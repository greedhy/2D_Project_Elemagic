// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 Sequencer view, filter, and curve-editor linkage tools.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelHandle.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "ScopedTransaction.h"
#include "SequencerSettings.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace UE::SOMOLMCP
{
namespace UE58Sequencer
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
static TSharedPtr<ISequencer> ResolveActiveSequencer(FString& Error)
{
	ULevelSequence* Sequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
	if (!Sequence)
	{
		Error = TEXT("No Level Sequence is open in the editor.");
		return nullptr;
	}
	if (!GEditor)
	{
		Error = TEXT("GEditor is unavailable.");
		return nullptr;
	}
	UAssetEditorSubsystem* EditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	IAssetEditorInstance* EditorInstance = EditorSubsystem
		? EditorSubsystem->FindEditorForAsset(Sequence, false)
		: nullptr;
	if (!EditorInstance || EditorInstance->GetEditorName() != FName(TEXT("LevelSequenceEditor")))
	{
		Error = FString::Printf(TEXT("The active Level Sequence editor instance was not found for %s."), *Sequence->GetPathName());
		return nullptr;
	}
	ILevelSequenceEditorToolkit* Toolkit = static_cast<ILevelSequenceEditorToolkit*>(EditorInstance);
	TSharedPtr<ISequencer> Sequencer = Toolkit ? Toolkit->GetSequencer() : nullptr;
	if (!Sequencer)
	{
		Error = TEXT("The active Level Sequence editor has no Sequencer instance.");
	}
	return Sequencer;
}

static void WriteSequenceIdentity(const TSharedPtr<ISequencer>& Sequencer, TSharedRef<FJsonObject>& Out)
{
	if (ULevelSequence* Sequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence())
	{
		Out->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	}
	Out->SetBoolField(TEXT("sequencer_valid"), Sequencer.IsValid());
}

static void WriteLinkSettings(USequencerSettings* Settings, TSharedRef<FJsonObject>& Out)
{
	Out->SetBoolField(TEXT("link_curve_editor_time_range"), Settings->GetLinkCurveEditorTimeRange());
	Out->SetBoolField(TEXT("sync_curve_editor_selection"), Settings->ShouldSyncCurveEditorSelection());
	Out->SetBoolField(TEXT("sync_outliner_selection"), Settings->ShouldSyncOutlinerSelectionToCurveEditor());
	Out->SetBoolField(TEXT("isolate_curve_editor_selection"), Settings->ShouldIsolateToCurveEditorSelection());
}

static bool Execute(
	const FString& Name,
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Args,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	(void)Context;
	TSharedPtr<ISequencer> Sequencer = ResolveActiveSequencer(Error);
	if (!Sequencer) return false;
	WriteSequenceIdentity(Sequencer, Out);

	if (Name == TEXT("sequencer_simple_view_settings_get"))
	{
		Out->SetBoolField(TEXT("simple_view"), Sequencer->IsSimpleView());
		Summary = TEXT("Read the active UE 5.8 Sequencer simplified-view state.");
		return true;
	}
	if (Name == TEXT("sequencer_simple_view_settings_set"))
	{
		bool bEnabled = false;
		if (!Args->TryGetBoolField(TEXT("enabled"), bEnabled))
		{
			Error = TEXT("enabled is required.");
			return false;
		}
		Sequencer->EnableSimpleView(bEnabled);
		const bool bReadback = Sequencer->IsSimpleView();
		Out->SetBoolField(TEXT("requested"), bEnabled);
		Out->SetBoolField(TEXT("simple_view"), bReadback);
		Out->SetBoolField(TEXT("readback_match"), bReadback == bEnabled);
		if (bReadback != bEnabled)
		{
			Error = TEXT("Sequencer simplified-view readback did not match the requested state.");
			return false;
		}
		Summary = FString::Printf(TEXT("Set active UE 5.8 Sequencer simplified view to %s."), bEnabled ? TEXT("enabled") : TEXT("disabled"));
		return true;
	}
	if (Name == TEXT("sequencer_simple_view_key_batch_edit"))
	{
		FString Action;
		if (!Args->TryGetStringField(TEXT("action"), Action) ||
			!(Action.Equals(TEXT("translate"), ESearchCase::IgnoreCase) ||
			  Action.Equals(TEXT("scale"), ESearchCase::IgnoreCase) ||
			  Action.Equals(TEXT("delete"), ESearchCase::IgnoreCase)))
		{
			Error = TEXT("action must be translate, scale, or delete.");
			return false;
		}
		double FrameDelta = 0.0;
		double Scale = 1.0;
		double PivotFrame = 0.0;
		Args->TryGetNumberField(TEXT("frame_delta"), FrameDelta);
		Args->TryGetNumberField(TEXT("scale"), Scale);
		Args->TryGetNumberField(TEXT("pivot_frame"), PivotFrame);
		if (Action.Equals(TEXT("scale"), ESearchCase::IgnoreCase) && Scale <= 0.0)
		{
			Error = TEXT("scale must be greater than zero.");
			return false;
		}
		FString SelectionScope = TEXT("selected");
		Args->TryGetStringField(TEXT("selection_scope"), SelectionScope);
		if (!(SelectionScope.Equals(TEXT("selected"), ESearchCase::IgnoreCase) ||
			SelectionScope.Equals(TEXT("all_keyed"), ESearchCase::IgnoreCase)))
		{
			Error = TEXT("selection_scope must be selected or all_keyed.");
			return false;
		}
		if (SelectionScope.Equals(TEXT("all_keyed"), ESearchCase::IgnoreCase))
		{
			double MaxKeysNumber = 10000.0;
			Args->TryGetNumberField(TEXT("max_keys"), MaxKeysNumber);
			const int32 MaxKeys = FMath::Clamp(FMath::FloorToInt(MaxKeysNumber), 1, 100000);
			ULevelSequence* Sequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
			UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
			if (!MovieScene)
			{
				Error = TEXT("The active Level Sequence has no MovieScene.");
				return false;
			}
			struct FChannelEdit
			{
				UMovieSceneSection* Section = nullptr;
				FMovieSceneChannel* Channel = nullptr;
				FString Name;
				TArray<FKeyHandle> Handles;
				TArray<FFrameNumber> Times;
			};
			TArray<FChannelEdit> Edits;
			int32 TotalKeys = 0;
			auto CollectTrackKeys = [&](UMovieSceneTrack* Track) -> bool
			{
				if (!Track) return true;
				for (UMovieSceneSection* Section : Track->GetAllSections())
				{
					if (!Section) continue;
					for (const FMovieSceneChannelEntry& Entry : Section->GetChannelProxy().GetAllEntries())
					{
						const TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();
						const TArrayView<const FMovieSceneChannelMetaData> Meta = Entry.GetMetaData();
						for (int32 ChannelIndex = 0; ChannelIndex < Channels.Num(); ++ChannelIndex)
						{
							FMovieSceneChannel* Channel = Channels[ChannelIndex];
							if (!Channel || Channel->GetNumKeys() <= 0) continue;
							FChannelEdit Edit;
							Edit.Section = Section;
							Edit.Channel = Channel;
							Edit.Name = Meta.IsValidIndex(ChannelIndex)
								? Meta[ChannelIndex].Name.ToString()
								: FString::Printf(TEXT("%s[%d]"), *Entry.GetChannelTypeName().ToString(), ChannelIndex);
							Channel->GetKeys(TRange<FFrameNumber>::All(), &Edit.Times, &Edit.Handles);
							TotalKeys += Edit.Handles.Num();
							if (TotalKeys > MaxKeys)
							{
								Error = FString::Printf(TEXT("all_keyed resolved %d keys, exceeding max_keys=%d."), TotalKeys, MaxKeys);
								return false;
							}
							Edits.Add(MoveTemp(Edit));
						}
					}
				}
				return true;
			};
			for (UMovieSceneTrack* Track : MovieScene->GetTracks())
			{
				if (!CollectTrackKeys(Track)) return false;
			}
			for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
			{
				for (UMovieSceneTrack* Track : Binding.GetTracks())
				{
					if (!CollectTrackKeys(Track)) return false;
				}
			}
			if (TotalKeys == 0)
			{
				Error = TEXT("The active Level Sequence contains no keyed channels.");
				return false;
			}
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequencerAllKeyedBatchEdit", "SOMOLMCP Sequencer All-Keyed Batch Edit"));
			TArray<TSharedPtr<FJsonValue>> ChannelRows;
			for (FChannelEdit& Edit : Edits)
			{
				Edit.Section->Modify();
				if (Action.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
				{
					Edit.Channel->DeleteKeys(Edit.Handles);
				}
				else
				{
					for (FFrameNumber& Time : Edit.Times)
					{
						const double NewFrame = Action.Equals(TEXT("translate"), ESearchCase::IgnoreCase)
							? static_cast<double>(Time.Value) + FrameDelta
							: PivotFrame + (static_cast<double>(Time.Value) - PivotFrame) * Scale;
						Time = FFrameNumber(FMath::RoundToInt32(NewFrame));
					}
					Edit.Channel->SetKeyTimes(Edit.Handles, Edit.Times);
				}
				Edit.Channel->PostEditChange();
				Edit.Section->MarkPackageDirty();
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("channel_name"), Edit.Name);
				Row->SetStringField(TEXT("section_path"), Edit.Section->GetPathName());
				Row->SetNumberField(TEXT("key_count"), Edit.Handles.Num());
				ChannelRows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);
			Out->SetStringField(TEXT("action"), Action.ToLower());
			Out->SetStringField(TEXT("selection_scope"), TEXT("all_keyed"));
			Out->SetNumberField(TEXT("edited_channel_count"), Edits.Num());
			Out->SetNumberField(TEXT("edited_key_count"), TotalKeys);
			Out->SetArrayField(TEXT("channels"), ChannelRows);
			Summary = FString::Printf(TEXT("Applied Sequencer %s to all %d key(s) across %d keyed channel(s)."),
				*Action.ToLower(), TotalKeys, Edits.Num());
			return true;
		}

		const TArray<FSequencerChannelProxy> SelectedChannels =
			ULevelSequenceEditorBlueprintLibrary::GetChannelsWithSelectedKeys();
		if (SelectedChannels.IsEmpty())
		{
			Error = TEXT("No Sequencer keys are selected.");
			return false;
		}

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequencerBatchEdit", "SOMOLMCP Sequencer Key Batch Edit"));
		int32 EditedChannelCount = 0;
		int32 EditedKeyCount = 0;
		TArray<TSharedPtr<FJsonValue>> ChannelRows;
		for (const FSequencerChannelProxy& Selected : SelectedChannels)
		{
			UMovieSceneSection* Section = Selected.Section.Get();
			if (!Section) continue;
			FMovieSceneChannelHandle ChannelHandle = Section->GetChannelProxy().GetChannelByName(Selected.ChannelName);
			FMovieSceneChannel* Channel = ChannelHandle.Get();
			if (!Channel) continue;

			const TArray<int32> SelectedIndices = ULevelSequenceEditorBlueprintLibrary::GetSelectedKeys(Selected);
			TArray<FFrameNumber> AllTimes;
			TArray<FKeyHandle> AllHandles;
			Channel->GetKeys(TRange<FFrameNumber>::All(), &AllTimes, &AllHandles);
			TArray<FKeyHandle> Handles;
			TArray<FFrameNumber> Times;
			for (const int32 Index : SelectedIndices)
			{
				if (!AllHandles.IsValidIndex(Index) || !AllTimes.IsValidIndex(Index))
				{
					Error = FString::Printf(TEXT("Selected key index %d is invalid for channel %s."), Index, *Selected.ChannelName.ToString());
					return false;
				}
				Handles.Add(AllHandles[Index]);
				Times.Add(AllTimes[Index]);
			}
			if (Handles.IsEmpty()) continue;

			Section->Modify();
			if (Action.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
			{
				Channel->DeleteKeys(Handles);
			}
			else
			{
				for (FFrameNumber& Time : Times)
				{
					const double NewFrame = Action.Equals(TEXT("translate"), ESearchCase::IgnoreCase)
						? static_cast<double>(Time.Value) + FrameDelta
						: PivotFrame + (static_cast<double>(Time.Value) - PivotFrame) * Scale;
					Time = FFrameNumber(FMath::RoundToInt32(NewFrame));
				}
				Channel->SetKeyTimes(Handles, Times);
			}
			Channel->PostEditChange();
			Section->MarkPackageDirty();
			++EditedChannelCount;
			EditedKeyCount += Handles.Num();

			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("channel_name"), Selected.ChannelName.ToString());
			Row->SetStringField(TEXT("section_path"), Section->GetPathName());
			Row->SetNumberField(TEXT("key_count"), Handles.Num());
			ChannelRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		if (EditedKeyCount == 0)
		{
			Error = TEXT("Selected Sequencer key channels resolved, but no editable key handles were found.");
			return false;
		}
		Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);
		Out->SetStringField(TEXT("action"), Action.ToLower());
		Out->SetStringField(TEXT("selection_scope"), TEXT("selected"));
		Out->SetNumberField(TEXT("edited_channel_count"), EditedChannelCount);
		Out->SetNumberField(TEXT("edited_key_count"), EditedKeyCount);
		Out->SetArrayField(TEXT("channels"), ChannelRows);
		Summary = FString::Printf(TEXT("Applied Sequencer %s to %d selected key(s) across %d channel(s)."),
			*Action.ToLower(), EditedKeyCount, EditedChannelCount);
		return true;
	}
	if (Name == TEXT("sequencer_filter_system_get") || Name == TEXT("sequencer_filter_system_set"))
	{
		const TArray<FText> FilterNames = ULevelSequenceEditorBlueprintLibrary::GetTrackFilterNames();
		if (Name == TEXT("sequencer_filter_system_set"))
		{
			FString RequestedName;
			bool bActive = false;
			if (!Args->TryGetStringField(TEXT("filter_name"), RequestedName) || RequestedName.IsEmpty())
			{
				Error = TEXT("filter_name is required.");
				return false;
			}
			if (!Args->TryGetBoolField(TEXT("active"), bActive))
			{
				Error = TEXT("active is required.");
				return false;
			}
			const FText* Match = FilterNames.FindByPredicate([&RequestedName](const FText& Candidate)
			{
				return Candidate.ToString().Equals(RequestedName, ESearchCase::IgnoreCase);
			});
			if (!Match)
			{
				Error = FString::Printf(TEXT("Unknown Sequencer track filter: %s"), *RequestedName);
				return false;
			}
			ULevelSequenceEditorBlueprintLibrary::SetTrackFilterActive(*Match, bActive);
			const bool bReadback = ULevelSequenceEditorBlueprintLibrary::IsTrackFilterActive(*Match);
			Out->SetStringField(TEXT("filter_name"), Match->ToString());
			Out->SetBoolField(TEXT("requested"), bActive);
			Out->SetBoolField(TEXT("active"), bReadback);
			Out->SetBoolField(TEXT("readback_match"), bReadback == bActive);
			if (bReadback != bActive)
			{
				Error = TEXT("Sequencer track-filter readback did not match the requested state.");
				return false;
			}
		}

		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FText& FilterName : FilterNames)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), FilterName.ToString());
			Row->SetBoolField(TEXT("active"), ULevelSequenceEditorBlueprintLibrary::IsTrackFilterActive(FilterName));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Out->SetArrayField(TEXT("filters"), Rows);
		Out->SetNumberField(TEXT("filter_count"), Rows.Num());
		Summary = Name.EndsWith(TEXT("_set"))
			? TEXT("Updated and read back an active UE 5.8 Sequencer track filter.")
			: FString::Printf(TEXT("Read %d UE 5.8 Sequencer track filters."), Rows.Num());
		return true;
	}
	if (Name == TEXT("sequencer_link_system_get") || Name == TEXT("sequencer_link_system_set"))
	{
		USequencerSettings* Settings = Sequencer->GetSequencerSettings();
		if (!Settings)
		{
			Error = TEXT("Sequencer settings are unavailable.");
			return false;
		}
		if (Name == TEXT("sequencer_link_system_set"))
		{
			bool bAny = false;
			bool bValue = false;
			if (Args->TryGetBoolField(TEXT("link_curve_editor_time_range"), bValue))
			{
				Settings->SetLinkCurveEditorTimeRange(bValue);
				bAny = true;
			}
			if (Args->TryGetBoolField(TEXT("sync_curve_editor_selection"), bValue))
			{
				Settings->SyncCurveEditorSelection(bValue);
				bAny = true;
			}
			if (Args->TryGetBoolField(TEXT("sync_outliner_selection"), bValue))
			{
				Settings->SyncOutlinerSelectionToCurveEditor(bValue);
				bAny = true;
			}
			if (Args->TryGetBoolField(TEXT("isolate_curve_editor_selection"), bValue))
			{
				Settings->IsolateCurveEditorToSelection(bValue);
				bAny = true;
			}
			if (!bAny)
			{
				Error = TEXT("At least one Sequencer link setting is required.");
				return false;
			}
			Settings->SaveConfig();
		}
		WriteLinkSettings(Settings, Out);
		Summary = Name.EndsWith(TEXT("_set"))
			? TEXT("Updated, persisted, and read back UE 5.8 Sequencer curve-editor linkage settings.")
			: TEXT("Read UE 5.8 Sequencer curve-editor linkage settings.");
		return true;
	}

	Error = FString::Printf(TEXT("Unsupported UE 5.8 Sequencer tool: %s"), *Name);
	return false;
}

static TSharedRef<FJsonObject> Schema()
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable or disable Sequencer simplified view."))},
		{TEXT("action"), FSololmcpSchemaBuilder::String(TEXT("Selected-key edit: translate, scale, or delete."))},
		{TEXT("frame_delta"), FSololmcpSchemaBuilder::Number(TEXT("Frame offset used by translate."))},
		{TEXT("scale"), FSololmcpSchemaBuilder::Number(TEXT("Positive time scale used by scale."))},
		{TEXT("pivot_frame"), FSololmcpSchemaBuilder::Number(TEXT("Frame pivot used by scale."))},
		{TEXT("selection_scope"), FSololmcpSchemaBuilder::String(TEXT("selected (default) or all_keyed."))},
		{TEXT("max_keys"), FSololmcpSchemaBuilder::Number(TEXT("Fail-closed maximum key count for all_keyed; default 10000, maximum 100000."))},
		{TEXT("filter_name"), FSololmcpSchemaBuilder::String(TEXT("Exact track-filter display name returned by sequencer_filter_system_get."))},
		{TEXT("active"), FSololmcpSchemaBuilder::Boolean(TEXT("Activate or deactivate the named track filter."))},
		{TEXT("link_curve_editor_time_range"), FSololmcpSchemaBuilder::Boolean(TEXT("Link Sequencer and Curve Editor time ranges."))},
		{TEXT("sync_curve_editor_selection"), FSololmcpSchemaBuilder::Boolean(TEXT("Synchronize Sequencer selection into Curve Editor."))},
		{TEXT("sync_outliner_selection"), FSololmcpSchemaBuilder::Boolean(TEXT("Synchronize Curve Editor selection into Sequencer outliner."))},
		{TEXT("isolate_curve_editor_selection"), FSololmcpSchemaBuilder::Boolean(TEXT("Isolate Curve Editor to current Sequencer selection."))}
	});
}
#endif
}

void RegisterUE58SequencerTools(FSololmcpToolRegistry& Registry)
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
	static const TCHAR* Names[] = {
		TEXT("sequencer_simple_view_settings_get"),
		TEXT("sequencer_simple_view_settings_set"),
		TEXT("sequencer_simple_view_key_batch_edit"),
		TEXT("sequencer_filter_system_get"),
		TEXT("sequencer_filter_system_set"),
		TEXT("sequencer_link_system_get"),
		TEXT("sequencer_link_system_set")
	};
	for (const TCHAR* NamePtr : Names)
	{
		const FString Name(NamePtr);
		FSololmcpToolDefinition Def;
		Def.Name = Name;
		Def.Description = FString::Printf(TEXT("UE 5.8 active Sequencer transaction: %s"), *Name);
		Def.InputSchema = UE58Sequencer::Schema();
		Def.CacheTtlSeconds = Name.EndsWith(TEXT("_get")) ? 1 : 0;
		Def.Execute = [Name](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return UE58Sequencer::Execute(Name, Context, Args, Out, Summary, Error);
		};
		Registry.Register(Def);
	}
#endif
}
}
