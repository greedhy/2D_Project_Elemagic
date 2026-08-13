// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// ControlRig coverage — Sequencer integration.
//
// UControlRigSequencerEditorLibrary is 130 BlueprintCallable entry points and was
// the largest remaining blank in the ControlRig domain: SOMOLMCP had two tools
// against it (binding create, bake to anim sequence), so nothing could enumerate
// which rigs a level sequence drives, or sample what those rigs actually produce
// over time.
//
// Not all 130 are reachable from JSON. Much of the library is keyed by live editor
// objects — UMovieSceneSection*, UTickableConstraint*, FMovieSceneBindingProxy —
// which have no stable addressable form outside an open Sequencer. The tools here
// cover the part that is addressable by asset path plus name, and the discovery
// tool exists specifically so a caller can find the rigs before operating on them.
//
// GetActorWorldTransforms is worth calling out: the engine already ships the batch
// form, taking an array of frames and returning an array of transforms. Sampling
// a motion curve is inherently a bulk read, and doing it frame by frame would cost
// one game-thread entry per frame against a small concurrent job budget.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "Services/SololmcpEditorServices.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "Runtime/Launch/Resources/Version.h"

#include "ControlRig.h"
#include "ControlRigSequencerEditorLibrary.h"
// FControlRigSequencerBindingProxy::Track is a TObjectPtr<UMovieSceneControlRigParameterTrack>,
// and the library header only forward-declares it; reading GetName off the track
// needs the complete type.
#include "Sequencer/MovieSceneControlRigParameterTrack.h"

namespace UE::SOMOLMCP
{
namespace RigSequencerToolsPrivate
{
	inline ULevelSequence* ResolveLevelSequence(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		const TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		FString SequencePath;
		if (!Args->TryGetStringField(TEXT("sequence_path"), SequencePath) || SequencePath.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("sequence_path"));
			OutError = TEXT("Missing sequence_path.");
			return nullptr;
		}
		ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(SequencePath, OutError));
		if (Sequence == nullptr)
		{
			SololmcpError::InvalidPath(OutStructured, SequencePath);
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not a Level Sequence."), *SequencePath);
			}
		}
		return Sequence;
	}

	inline TSharedRef<FJsonObject> TransformToJson(const FTransform& Transform)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		const FVector Location = Transform.GetLocation();
		const FRotator Rotation = Transform.Rotator();
		const FVector Scale = Transform.GetScale3D();

		TSharedRef<FJsonObject> LocationJson = MakeShared<FJsonObject>();
		LocationJson->SetNumberField(TEXT("x"), Location.X);
		LocationJson->SetNumberField(TEXT("y"), Location.Y);
		LocationJson->SetNumberField(TEXT("z"), Location.Z);
		Json->SetObjectField(TEXT("location"), LocationJson);

		TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>();
		RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
		RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
		RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
		Json->SetObjectField(TEXT("rotation"), RotationJson);

		TSharedRef<FJsonObject> ScaleJson = MakeShared<FJsonObject>();
		ScaleJson->SetNumberField(TEXT("x"), Scale.X);
		ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
		ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
		Json->SetObjectField(TEXT("scale"), ScaleJson);
		return Json;
	}

	inline TSharedRef<FJsonObject> SequenceArgSchema()
	{
		return FSololmcpSchemaBuilder::String(TEXT("Object path of the Level Sequence."));
	}
} // namespace RigSequencerToolsPrivate

void RegisterRigSequencerTools(FSololmcpToolRegistry& Registry)
{
	using namespace RigSequencerToolsPrivate;

	// ── control_rig_sequencer_binding_list ─────────────────────────────────
	Registry.Register({
		TEXT("control_rig_sequencer_binding_list"),
		TEXT("List the Control Rigs a Level Sequence drives, with their track names. Call this first: "
			 "the other sequencer rig tools address a rig by the name reported here, and nothing else "
			 "exposes which rigs a sequence actually contains."),
		FSololmcpSchemaBuilder::Object(
			{{TEXT("sequence_path"), SequenceArgSchema()}}, {TEXT("sequence_path")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			ULevelSequence* Sequence = ResolveLevelSequence(Context, Args, OutStructured, OutError);
			if (Sequence == nullptr)
			{
				return false;
			}

			const TArray<FControlRigSequencerBindingProxy> Bindings =
				UControlRigSequencerEditorLibrary::GetControlRigs(Sequence);

			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FControlRigSequencerBindingProxy& Binding : Bindings)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				if (Binding.ControlRig != nullptr)
				{
					Row->SetStringField(TEXT("control_rig_name"), Binding.ControlRig->GetName());
					Row->SetStringField(TEXT("control_rig_class"), Binding.ControlRig->GetClass()->GetPathName());
				}
				else
				{
					// A binding with no rig is a real state (unresolved spawnable), and
					// silently dropping it would make the list disagree with the track count.
					Row->SetStringField(TEXT("control_rig_name"), FString());
					Row->SetBoolField(TEXT("control_rig_unresolved"), true);
				}
				if (Binding.Track != nullptr)
				{
					Row->SetStringField(TEXT("track_name"), Binding.Track->GetName());
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
			OutStructured->SetArrayField(TEXT("bindings"), Rows);
			OutStructured->SetNumberField(TEXT("binding_count"), Rows.Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("'%s' drives %d Control Rig binding(s)."),
				*Sequence->GetName(), Rows.Num());
			return true;
		},
		nullptr,
		5
	});

	// ── control_rig_sequencer_visible_rigs_list ────────────────────────────
	Registry.Register({
		TEXT("control_rig_sequencer_visible_rigs_list"),
		TEXT("List the Control Rigs currently visible to the editor's Sequencer. Unlike "
			 "control_rig_sequencer_binding_list this reflects live editor state rather than a "
			 "sequence asset, so it is the way to confirm a rig is actually loaded before "
			 "operating on it."),
		FSololmcpSchemaBuilder::Object({}, {}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			const TArray<UControlRig*> Rigs = UControlRigSequencerEditorLibrary::GetVisibleControlRigs();

			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const UControlRig* Rig : Rigs)
			{
				if (Rig == nullptr)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Rig->GetName());
				Row->SetStringField(TEXT("class"), Rig->GetClass()->GetPathName());
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("control_rigs"), Rows);
			OutStructured->SetNumberField(TEXT("count"), Rows.Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d Control Rig(s) visible to Sequencer."), Rows.Num());
			return true;
		},
		nullptr,
		0
	});

	// ── control_rig_sequencer_actor_transforms_sample ──────────────────────
	Registry.Register({
		TEXT("control_rig_sequencer_actor_transforms_sample"),
		TEXT("Sample an actor's world transform across many frames of a Level Sequence in ONE "
			 "game-thread entry. This is how motion is read out of a sequence for analysis, "
			 "retargeting or validation — sampling frame by frame would cost one entry per frame "
			 "against a small concurrent job budget."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("sequence_path"), SequenceArgSchema()},
				{TEXT("actor"), FSololmcpSchemaBuilder::String(
					TEXT("Actor label, name or path in the current level."))},
				{TEXT("frames"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Integer(TEXT("Frame number.")),
					TEXT("Explicit frame list. Use this or start_frame/end_frame."))},
				{TEXT("start_frame"), FSololmcpSchemaBuilder::Integer(
					TEXT("First frame of a generated range."))},
				{TEXT("end_frame"), FSololmcpSchemaBuilder::Integer(
					TEXT("Last frame of a generated range, inclusive."))},
				{TEXT("step"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(TEXT("Frame step for a generated range.")), 1)},
				{TEXT("max_frames"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(
						TEXT("Safety cap; a long range evaluates the sequence once per frame.")), 2000)}
			},
			{TEXT("sequence_path"), TEXT("actor")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			ULevelSequence* Sequence = ResolveLevelSequence(Context, Args, OutStructured, OutError);
			if (Sequence == nullptr)
			{
				return false;
			}

			FString ActorId;
			if (!Args->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("actor"));
				OutError = TEXT("Missing actor.");
				return false;
			}
			AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
			if (Actor == nullptr)
			{
				SololmcpError::NotFound(OutStructured, ActorId);
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(TEXT("No actor '%s' in the current level."), *ActorId);
				}
				return false;
			}

			int32 MaxFrames = 2000;
			Args->TryGetNumberField(TEXT("max_frames"), MaxFrames);
			MaxFrames = FMath::Clamp(MaxFrames, 1, 100000);

			TArray<FFrameNumber> Frames;
			const TArray<TSharedPtr<FJsonValue>>* Explicit = nullptr;
			if (Args->TryGetArrayField(TEXT("frames"), Explicit) && Explicit != nullptr && Explicit->Num() > 0)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Explicit)
				{
					int32 Frame = 0;
					if (Value.IsValid() && Value->TryGetNumber(Frame))
					{
						Frames.Add(FFrameNumber(Frame));
					}
					if (Frames.Num() >= MaxFrames)
					{
						break;
					}
				}
			}
			else
			{
				int32 Start = 0;
				int32 End = 0;
				if (!Args->TryGetNumberField(TEXT("start_frame"), Start)
					|| !Args->TryGetNumberField(TEXT("end_frame"), End))
				{
					SololmcpError::Set(OutStructured, TEXT("MISSING_PARAM"), TEXT("frames"),
						TEXT("Pass frames, or both start_frame and end_frame."));
					OutError = TEXT("No frames requested.");
					return false;
				}
				int32 Step = 1;
				Args->TryGetNumberField(TEXT("step"), Step);
				Step = FMath::Max(1, Step);
				if (End < Start)
				{
					SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("end_frame"),
						TEXT("end_frame must be greater than or equal to start_frame."));
					OutError = TEXT("Invalid frame range.");
					return false;
				}
				for (int32 Frame = Start; Frame <= End && Frames.Num() < MaxFrames; Frame += Step)
				{
					Frames.Add(FFrameNumber(Frame));
				}
			}

			if (Frames.Num() == 0)
			{
				SololmcpError::Set(OutStructured, TEXT("MISSING_PARAM"), TEXT("frames"),
					TEXT("The request resolved to zero frames."));
				OutError = TEXT("No frames to sample.");
				return false;
			}

			const TArray<FTransform> Transforms =
				UControlRigSequencerEditorLibrary::GetActorWorldTransforms(Sequence, Actor, Frames);

			TArray<TSharedPtr<FJsonValue>> Rows;
			const int32 Count = FMath::Min(Frames.Num(), Transforms.Num());
			for (int32 Index = 0; Index < Count; ++Index)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("frame"), Frames[Index].Value);
				Row->SetObjectField(TEXT("transform"), TransformToJson(Transforms[Index]));
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
			OutStructured->SetStringField(TEXT("actor"), Actor->GetName());
			OutStructured->SetArrayField(TEXT("samples"), Rows);
			OutStructured->SetNumberField(TEXT("requested_frames"), Frames.Num());
			OutStructured->SetNumberField(TEXT("returned_samples"), Rows.Num());
			OutStructured->SetNumberField(TEXT("game_thread_entries"), 1);
			OutStructured->SetBoolField(TEXT("ok"), true);
			if (Transforms.Num() != Frames.Num())
			{
				// A short result means the sequence could not evaluate every frame;
				// reporting the mismatch keeps a truncated curve from reading as complete.
				OutStructured->SetBoolField(TEXT("incomplete"), true);
				OutStructured->SetStringField(TEXT("incomplete_reason"),
					TEXT("The sequence returned fewer transforms than frames requested; frames outside "
						 "the sequence's playback range do not evaluate."));
			}
			OutSummary = FString::Printf(TEXT("Sampled %d frame(s) of '%s' in one game-thread entry."),
				Rows.Num(), *Actor->GetName());
			return true;
		},
		nullptr,
		0
	});

	// ── control_rig_sequencer_tween ────────────────────────────────────────
	Registry.Register({
		TEXT("control_rig_sequencer_tween"),
		TEXT("Blend a Control Rig's selected controls toward their neighbouring keys. "
			 "tween_value runs -1 to 1: negative pulls toward the previous key, positive toward the "
			 "next, 0 leaves the pose unchanged. Operates on the current control selection, so "
			 "select controls in the editor first."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("sequence_path"), SequenceArgSchema()},
				{TEXT("control_rig_name"), FSololmcpSchemaBuilder::String(
					TEXT("Rig name as reported by control_rig_sequencer_binding_list."))},
				{TEXT("tween_value"), FSololmcpSchemaBuilder::Number(
					TEXT("Blend amount from -1 (previous key) to 1 (next key)."), -1.0, 1.0)}
			},
			{TEXT("sequence_path"), TEXT("control_rig_name"), TEXT("tween_value")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			ULevelSequence* Sequence = ResolveLevelSequence(Context, Args, OutStructured, OutError);
			if (Sequence == nullptr)
			{
				return false;
			}

			FString RigName;
			if (!Args->TryGetStringField(TEXT("control_rig_name"), RigName) || RigName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("control_rig_name"));
				OutError = TEXT("Missing control_rig_name.");
				return false;
			}
			double TweenValue = 0.0;
			if (!Args->TryGetNumberField(TEXT("tween_value"), TweenValue))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("tween_value"));
				OutError = TEXT("Missing tween_value.");
				return false;
			}

			UControlRig* Target = nullptr;
			TArray<FString> Available;
			for (const FControlRigSequencerBindingProxy& Binding
				: UControlRigSequencerEditorLibrary::GetControlRigs(Sequence))
			{
				if (Binding.ControlRig == nullptr)
				{
					continue;
				}
				Available.Add(Binding.ControlRig->GetName());
				if (Binding.ControlRig->GetName() == RigName)
				{
					Target = Binding.ControlRig;
				}
			}
			if (Target == nullptr)
			{
				TArray<TSharedPtr<FJsonValue>> Rows;
				for (const FString& Name : Available)
				{
					Rows.Add(MakeShared<FJsonValueString>(Name));
				}
				OutStructured->SetArrayField(TEXT("available_control_rigs"), Rows);
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("control_rig_name"),
					TEXT("Use a name from available_control_rigs, or run control_rig_sequencer_binding_list."));
				OutError = FString::Printf(TEXT("No Control Rig named '%s' in this sequence."), *RigName);
				return false;
			}

			const bool bTweened = UControlRigSequencerEditorLibrary::TweenControlRig(
				Sequence, Target, static_cast<float>(TweenValue));

			OutStructured->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
			OutStructured->SetStringField(TEXT("control_rig_name"), RigName);
			OutStructured->SetNumberField(TEXT("tween_value"), TweenValue);
			OutStructured->SetBoolField(TEXT("ok"), bTweened);
			if (!bTweened)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("control_rig_name"),
					TEXT("Tween operates on the current control selection and on keyed controls; "
						 "with nothing selected or no surrounding keys there is nothing to blend."));
				OutError = FString::Printf(TEXT("Tween did not apply to '%s'."), *RigName);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Tweened '%s' by %.3f."), *RigName, TweenValue);
			return true;
		},
		nullptr,
		0
	});
}

} // namespace UE::SOMOLMCP
