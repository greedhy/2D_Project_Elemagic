// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 Control Rig Physics graph authoring tools.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"

#if SOMOLMCP_WITH_UE58_MESHPARTITION

#include "ControlRigBlueprintEditorLibrary.h"
#include "ControlRigBlueprintLegacy.h"
#include "ControlRigSequencerEditorLibrary.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelSequence.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneBindingProxy.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSpawnable.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "RigPhysicsBodyExecution.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMEditorBlueprintLibrary.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Units/Hierarchy/RigUnit_GetControlTransform.h"

namespace UE::SOMOLMCP
{
namespace UE58ControlRigPhysics
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
static UControlRigBlueprint* LoadRig(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Args,
	FString& AssetPath,
	FString& Error)
{
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Error = TEXT("asset_path is required.");
		return nullptr;
	}
	UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, Error));
	if (!Rig && Error.IsEmpty()) Error = FString::Printf(TEXT("Control Rig asset was not found: %s"), *AssetPath);
	return Rig;
}

static URigVMController* GetController(UControlRigBlueprint* Rig, FString& Error)
{
	URigVMController* Controller = Rig ? URigVMEditorBlueprintLibrary::GetController(Rig) : nullptr;
	if (!Controller) Error = TEXT("RigVM controller is unavailable for the Control Rig asset.");
	return Controller;
}

static URigVMUnitNode* FindForceNode(URigVMController* Controller, const FString& NodeName, FString& Error)
{
	URigVMGraph* Graph = Controller ? Controller->GetGraph() : nullptr;
	URigVMNode* Node = Graph ? Graph->FindNode(NodeName) : nullptr;
	if (!Node && Graph) Node = Graph->FindNodeByName(FName(*NodeName));
	URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node);
	if (!UnitNode || UnitNode->GetScriptStruct() != FRigUnit_HierarchyAddPhysicsBodyForce::StaticStruct())
	{
		Error = FString::Printf(TEXT("Control Rig Physics Add Force node was not found: %s"), *NodeName);
		return nullptr;
	}
	return UnitNode;
}

static ULevelSequence* LoadSequence(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Args,
	FString& SequencePath,
	FString& Error)
{
	if (!Args->TryGetStringField(TEXT("sequence_path"), SequencePath) || SequencePath.IsEmpty())
	{
		Error = TEXT("sequence_path is required.");
		return nullptr;
	}
	ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(SequencePath, Error));
	if (!Sequence && Error.IsEmpty()) Error = FString::Printf(TEXT("Level Sequence was not found: %s"), *SequencePath);
	return Sequence;
}

static UMovieSceneControlRigParameterTrack* FindControlRigTrack(
	ULevelSequence* Sequence,
	UControlRigBlueprint* Rig,
	const FString& TrackName)
{
	if (!Sequence || !Rig) return nullptr;
	for (const FControlRigSequencerBindingProxy& Binding : UControlRigSequencerEditorLibrary::GetControlRigs(Sequence))
	{
		UMovieSceneControlRigParameterTrack* Track = Binding.Track;
		UControlRig* Instance = Binding.ControlRig;
		if (!Track || !Instance) continue;
		const bool bAssetMatch = Instance->GetClass() && Instance->GetClass()->ClassGeneratedBy == Rig;
		const bool bNameMatch = TrackName.IsEmpty()
			|| Track->GetTrackName().ToString().Equals(TrackName, ESearchCase::IgnoreCase)
			|| Track->GetDisplayName().ToString().Equals(TrackName, ESearchCase::IgnoreCase);
		if (bAssetMatch && bNameMatch) return Track;
	}
	return nullptr;
}

static int32 CountControlKeys(UMovieSceneControlRigParameterTrack* Track, const FString& ControlName)
{
	int32 Count = 0;
	if (!Track) return Count;
	for (UMovieSceneSection* Section : Track->GetAllSections())
	{
		if (!Section) continue;
		if (const UMovieSceneControlRigParameterSection* RigSection = Cast<UMovieSceneControlRigParameterSection>(Section))
		{
			for (const FVectorParameterNameAndCurves& Vector : RigSection->GetVectorParameterNamesAndCurves())
			{
				if (Vector.ParameterName == FName(*ControlName))
				{
					return Vector.XCurve.GetNumKeys() + Vector.YCurve.GetNumKeys() + Vector.ZCurve.GetNumKeys();
				}
			}
		}
		for (const FMovieSceneChannelEntry& Entry : Section->GetChannelProxy().GetAllEntries())
		{
			const TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();
			const TArrayView<const FMovieSceneChannelMetaData> Meta = Entry.GetMetaData();
			for (int32 Index = 0; Index < Channels.Num(); ++Index)
			{
				if (!Channels[Index]) continue;
				const FString ChannelName = Meta.IsValidIndex(Index) ? Meta[Index].Name.ToString() : FString();
				if (ChannelName.Contains(ControlName, ESearchCase::IgnoreCase)) Count += Channels[Index]->GetNumKeys();
			}
		}
	}
	return Count;
}

static bool WritePositionControlKeys(
	ULevelSequence* Sequence,
	UMovieSceneControlRigParameterTrack* Track,
	const FString& ControlName,
	const TArray<FFrameNumber>& DisplayFrames,
	const TArray<FVector>& Values,
	FString& Error)
{
	if (!Sequence || !Sequence->GetMovieScene() || !Track || DisplayFrames.Num() != Values.Num())
	{
		Error = TEXT("Invalid Level Sequence, Control Rig track, or animation sample arrays.");
		return false;
	}
	UMovieSceneControlRigParameterSection* RigSection = nullptr;
	for (UMovieSceneSection* Section : Track->GetAllSections())
	{
		if (UMovieSceneControlRigParameterSection* Candidate = Cast<UMovieSceneControlRigParameterSection>(Section))
		{
			RigSection = Candidate;
			break;
		}
	}
	if (!RigSection)
	{
		RigSection = Cast<UMovieSceneControlRigParameterSection>(Track->CreateNewSection());
		if (!RigSection)
		{
			Error = TEXT("Failed to create a native Control Rig parameter section.");
			return false;
		}
		Track->AddSection(*RigSection);
	}
	const FName ParameterName(*ControlName);
	if (!RigSection->HasVectorParameter(ParameterName))
	{
		RigSection->AddVectorParameter(ParameterName, FVector::ZeroVector, true);
	}
	FVectorParameterNameAndCurves* Curves = nullptr;
	for (FVectorParameterNameAndCurves& Candidate : RigSection->GetVectorParameterNamesAndCurves())
	{
		if (Candidate.ParameterName == ParameterName)
		{
			Curves = &Candidate;
			break;
		}
	}
	if (!Curves)
	{
		Error = FString::Printf(TEXT("Control Rig section did not expose vector channels for %s."), *ControlName);
		return false;
	}
	const FFrameRate TickResolution = Sequence->GetMovieScene()->GetTickResolution();
	const FFrameRate DisplayRate = Sequence->GetMovieScene()->GetDisplayRate();
	for (int32 Index = 0; Index < DisplayFrames.Num(); ++Index)
	{
		const FFrameNumber TickFrame = FFrameRate::TransformTime(
			FFrameTime(DisplayFrames[Index], 0), DisplayRate, TickResolution).RoundToFrame();
		Curves->XCurve.GetData().UpdateOrAddKey(TickFrame, FMovieSceneFloatValue(Values[Index].X));
		Curves->YCurve.GetData().UpdateOrAddKey(TickFrame, FMovieSceneFloatValue(Values[Index].Y));
		Curves->ZCurve.GetData().UpdateOrAddKey(TickFrame, FMovieSceneFloatValue(Values[Index].Z));
		RigSection->ExpandToFrame(TickFrame);
	}
	RigSection->ReconstructChannelProxy();
	RigSection->MarkPackageDirty();
	Track->MarkPackageDirty();
	return true;
}

static bool ParseAnimationSamples(
	const TSharedRef<FJsonObject>& Args,
	TArray<FFrameNumber>& Frames,
	TArray<FVector>& Values,
	FString& Error)
{
	const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
	if (!Args->TryGetArrayField(TEXT("samples"), Samples) || !Samples || Samples->IsEmpty())
	{
		Error = TEXT("samples must contain at least one {frame,x,y,z} object.");
		return false;
	}
	for (int32 Index = 0; Index < Samples->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* Sample = nullptr;
		if (!(*Samples)[Index].IsValid() || !(*Samples)[Index]->TryGetObject(Sample) || !Sample || !Sample->IsValid())
		{
			Error = FString::Printf(TEXT("samples[%d] must be an object."), Index);
			return false;
		}
		double Frame = 0.0, X = 0.0, Y = 0.0, Z = 0.0;
		if (!(*Sample)->TryGetNumberField(TEXT("frame"), Frame)
			|| !(*Sample)->TryGetNumberField(TEXT("x"), X)
			|| !(*Sample)->TryGetNumberField(TEXT("y"), Y)
			|| !(*Sample)->TryGetNumberField(TEXT("z"), Z))
		{
			Error = FString::Printf(TEXT("samples[%d] requires numeric frame, x, y, and z."), Index);
			return false;
		}
		Frames.Add(FFrameNumber(FMath::RoundToInt32(Frame)));
		Values.Add(FVector(X, Y, Z));
	}
	return true;
}

static bool AddInlineScreenshotReceipt(
	const FSololmcpToolExecutionContext& Context,
	const FString& AssetPath,
	int32 MaxWidth,
	int32 MaxHeight,
	TSharedRef<FJsonObject>& Out,
	FString& Error)
{
	TArray<uint8> PngData;
	if (!Context.Services.CaptureViewportScreenshot(PngData, MaxWidth, MaxHeight, Error) || PngData.Num() < 24)
	{
		if (Error.IsEmpty()) Error = TEXT("Control Rig Physics preview capture returned an invalid PNG payload.");
		return false;
	}
	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("ControlRigPhysicsPreviews")));
	if (!IFileManager::Get().MakeDirectory(*Directory, true))
	{
		Error = FString::Printf(TEXT("Failed to create preview directory: %s"), *Directory);
		return false;
	}
	FString AssetToken = FPaths::GetBaseFilename(AssetPath);
	const FString FilePath = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%s.png"),
		*AssetToken, *FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ"))));
	if (!FFileHelper::SaveArrayToFile(PngData, *FilePath))
	{
		Error = FString::Printf(TEXT("Failed to save Control Rig Physics preview: %s"), *FilePath);
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> ImageContent;
	ImageContent.Add(MakeImageContentValue(PngData));
	Out->SetArrayField(TEXT("_imageContent"), ImageContent);
	Out->SetStringField(TEXT("preview_file"), FilePath);
	Out->SetStringField(TEXT("preview_mime_type"), TEXT("image/png"));
	Out->SetNumberField(TEXT("preview_size_bytes"), PngData.Num());
	Out->SetNumberField(TEXT("preview_width"),
		(static_cast<int32>(PngData[16]) << 24) | (static_cast<int32>(PngData[17]) << 16)
		| (static_cast<int32>(PngData[18]) << 8) | static_cast<int32>(PngData[19]));
	Out->SetNumberField(TEXT("preview_height"),
		(static_cast<int32>(PngData[20]) << 24) | (static_cast<int32>(PngData[21]) << 16)
		| (static_cast<int32>(PngData[22]) << 8) | static_cast<int32>(PngData[23]));
	return true;
}

static TSharedRef<FJsonObject> DescribeNode(URigVMUnitNode* Node)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_name"), Node->GetName());
	Result->SetStringField(TEXT("node_path"), Node->GetNodePath());
	Result->SetStringField(TEXT("struct_path"), Node->GetScriptStruct()->GetPathName());
	TArray<TSharedPtr<FJsonValue>> Pins;
	for (URigVMPin* Pin : Node->GetPins())
	{
		if (!Pin) continue;
		TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
		PinJson->SetStringField(TEXT("name"), Pin->GetName());
		PinJson->SetStringField(TEXT("path"), Pin->GetPinPath());
		PinJson->SetStringField(TEXT("cpp_type"), Pin->GetCPPType());
		PinJson->SetStringField(TEXT("default_value"), Pin->GetDefaultValue());
		Pins.Add(MakeShared<FJsonValueObject>(PinJson));
	}
	Result->SetArrayField(TEXT("pins"), Pins);
	Result->SetNumberField(TEXT("pin_count"), Pins.Num());
	return Result;
}

static bool CompileAndSave(
	const FSololmcpToolExecutionContext& Context,
	UControlRigBlueprint* Rig,
	const FString& AssetPath,
	TSharedRef<FJsonObject>& Out,
	FString& Error)
{
	FCompilerResultsLog CompileLog;
	FKismetEditorUtilities::CompileBlueprint(Rig, EBlueprintCompileOptions::None, &CompileLog);
	const bool bCompiled = Rig->Status != BS_Error && CompileLog.NumErrors == 0;
	Out->SetBoolField(TEXT("compiled"), bCompiled);
	Out->SetNumberField(TEXT("compile_errors"), CompileLog.NumErrors);
	Out->SetNumberField(TEXT("compile_warnings"), CompileLog.NumWarnings);
	if (!bCompiled)
	{
		Error = FString::Printf(TEXT("Control Rig Physics compile failed with %d error(s)."), CompileLog.NumErrors);
		return false;
	}
	if (!Context.Services.SaveAsset(AssetPath, false, Error)) return false;
	Out->SetBoolField(TEXT("saved"), true);
	return true;
}

static bool Execute(
	const FString& Name,
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Args,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	if (Name == TEXT("control_rig_physics_capability_probe"))
	{
		Out->SetBoolField(TEXT("available"), true);
		Out->SetStringField(TEXT("module"), TEXT("ControlRigPhysics"));
		Out->SetStringField(TEXT("add_force_struct"), FRigUnit_HierarchyAddPhysicsBodyForce::StaticStruct()->GetPathName());
		Out->SetStringField(TEXT("remove_force_struct"), FRigUnit_HierarchyRemovePhysicsBodyForce::StaticStruct()->GetPathName());
		Out->SetBoolField(TEXT("graph_authoring_supported"), true);
		Summary = TEXT("UE 5.8 Control Rig Physics graph authoring is available.");
		return true;
	}

	FString AssetPath;
	UControlRigBlueprint* Rig = LoadRig(Context, Args, AssetPath, Error);
	if (!Rig) return false;
	URigVMController* Controller = GetController(Rig, Error);
	if (!Controller) return false;
	Out->SetStringField(TEXT("asset_path"), AssetPath);

	if (Name == TEXT("control_rig_physics_force_add"))
	{
		double X = 0.0;
		double Y = 0.0;
		Args->TryGetNumberField(TEXT("position_x"), X);
		Args->TryGetNumberField(TEXT("position_y"), Y);
		URigVMUnitNode* Node = Controller->AddUnitNode(
			FRigUnit_HierarchyAddPhysicsBodyForce::StaticStruct(),
			FRigUnit::GetMethodName(),
			FVector2D(static_cast<float>(X), static_cast<float>(Y)),
			FString(), true, false);
		if (!Node)
		{
			Error = TEXT("Failed to add the UE 5.8 Control Rig Physics Add Force node.");
			return false;
		}
		FString ForceName;
		if (Args->TryGetStringField(TEXT("force_name"), ForceName) && !ForceName.IsEmpty())
		{
			Controller->SetPinDefaultValue(Node->GetName() + TEXT(".Name"), ForceName, true, true, false, false, true);
		}
		Out->SetObjectField(TEXT("node"), DescribeNode(Node));
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		Summary = FString::Printf(TEXT("Added, compiled, saved, and read back Control Rig Physics force node %s."), *Node->GetName());
		return true;
	}
	if (Name == TEXT("control_rig_physics_force_update"))
	{
		FString NodeName;
		if (!Args->TryGetStringField(TEXT("node_name"), NodeName) || NodeName.IsEmpty())
		{
			Error = TEXT("node_name is required.");
			return false;
		}
		URigVMUnitNode* Node = FindForceNode(Controller, NodeName, Error);
		if (!Node) return false;
		const TSharedPtr<FJsonObject>* PinValues = nullptr;
		if (!Args->TryGetObjectField(TEXT("pin_values"), PinValues) || !PinValues || (*PinValues)->Values.IsEmpty())
		{
			Error = TEXT("pin_values must contain at least one top-level or nested pin value string.");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Updated;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PinValues)->Values)
		{
			FString Value;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Value))
			{
				Error = FString::Printf(TEXT("pin_values.%s must be a string."), *Pair.Key);
				return false;
			}
			const FString PinPath = Node->GetName() + TEXT(".") + Pair.Key;
			if (!Node->FindPin(Pair.Key))
			{
				Error = FString::Printf(TEXT("Pin is unavailable on the Add Force node: %s"), *Pair.Key);
				return false;
			}
			if (!Controller->SetPinDefaultValue(PinPath, Value, true, true, false, false, true))
			{
				Error = FString::Printf(TEXT("Failed to set Control Rig Physics pin: %s"), *PinPath);
				return false;
			}
			Updated.Add(MakeShared<FJsonValueString>(PinPath));
		}
		Out->SetArrayField(TEXT("updated_pins"), Updated);
		Out->SetObjectField(TEXT("node"), DescribeNode(Node));
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		Summary = FString::Printf(TEXT("Updated %d Control Rig Physics force pin(s), compiled, and saved."), Updated.Num());
		return true;
	}
	if (Name == TEXT("control_rig_physics_force_animate"))
	{
		FString NodeName, ControlName, SequencePath, BindingGuidText, BindingName;
		if (!Args->TryGetStringField(TEXT("node_name"), NodeName) || NodeName.IsEmpty())
		{
			Error = TEXT("node_name is required.");
			return false;
		}
		if (!Args->TryGetStringField(TEXT("control_name"), ControlName) || ControlName.IsEmpty())
		{
			Error = TEXT("control_name is required.");
			return false;
		}
		URigVMUnitNode* ForceNode = FindForceNode(Controller, NodeName, Error);
		if (!ForceNode) return false;
		ULevelSequence* Sequence = LoadSequence(Context, Args, SequencePath, Error);
		if (!Sequence) return false;
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			Error = FString::Printf(TEXT("Level Sequence has no MovieScene: %s."), *SequencePath);
			return false;
		}
		FGuid BindingGuid;
		if (Args->TryGetStringField(TEXT("binding_guid"), BindingGuidText) && !BindingGuidText.IsEmpty())
		{
			if (!FGuid::Parse(BindingGuidText, BindingGuid))
			{
				Error = TEXT("binding_guid is not a valid GUID.");
				return false;
			}
			if (!MovieScene->FindBinding(BindingGuid))
			{
				Error = FString::Printf(TEXT("binding_guid is not present in Level Sequence %s."), *SequencePath);
				return false;
			}
		}
		else if (Args->TryGetStringField(TEXT("binding_name"), BindingName) && !BindingName.IsEmpty())
		{
			int32 MatchCount = 0;
			const UMovieScene* ConstMovieScene = MovieScene;
			for (const FMovieSceneBinding& Candidate : ConstMovieScene->GetBindings())
			{
				const FGuid CandidateGuid = Candidate.GetObjectGuid();
				FString CandidateName;
				if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(CandidateGuid))
				{
					CandidateName = Possessable->GetName();
				}
				else if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(CandidateGuid))
				{
					CandidateName = Spawnable->GetName();
				}
				if (CandidateName.Equals(BindingName, ESearchCase::CaseSensitive))
				{
					BindingGuid = CandidateGuid;
					++MatchCount;
				}
			}
			if (MatchCount != 1)
			{
				Error = FString::Printf(TEXT("binding_name must match exactly one Level Sequence binding; found %d for '%s'."), MatchCount, *BindingName);
				return false;
			}
		}
		else
		{
			Error = TEXT("binding_guid or an exact unique binding_name is required so the Control Rig track cannot target the wrong actor.");
			return false;
		}

		URigHierarchyController* HierarchyController = UControlRigBlueprintEditorLibrary::GetHierarchyController(Rig);
		URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(Rig);
		if (!HierarchyController || !Hierarchy)
		{
			Error = TEXT("Control Rig hierarchy controller is unavailable.");
			return false;
		}
		const FRigElementKey ControlKey(FName(*ControlName), ERigElementType::Control);
		bool bCreatedControl = false;
		if (!Hierarchy->Find(ControlKey))
		{
			FRigControlSettings Settings;
			Settings.ControlType = ERigControlType::Position;
			Settings.DisplayName = FName(*ControlName);
			const FRigElementKey Created = HierarchyController->AddControl(
				FName(*ControlName), FRigElementKey(), Settings, FRigControlValue::Make(FVector::ZeroVector),
				FTransform::Identity, FTransform::Identity, true, false);
			if (!Created.IsValid())
			{
				Error = FString::Printf(TEXT("Failed to create Position control %s."), *ControlName);
				return false;
			}
			bCreatedControl = true;
		}
		else
		{
			const FRigControlSettings Settings = HierarchyController->GetControlSettings(ControlKey);
			if (Settings.ControlType != ERigControlType::Position)
			{
				Error = FString::Printf(TEXT("Existing control %s is not a Position control."), *ControlName);
				return false;
			}
		}

		URigVMPin* ForceLinearPin = ForceNode->FindPin(TEXT("ForceAndTorque.Linear"));
		if (!ForceLinearPin)
		{
			Error = TEXT("The Add Force node has no ForceAndTorque.Linear input pin.");
			return false;
		}
		URigVMUnitNode* GetterNode = nullptr;
		const TArray<URigVMPin*> ExistingSources = ForceLinearPin->GetLinkedSourcePins(true);
		for (URigVMPin* SourcePin : ExistingSources)
		{
			if (URigVMUnitNode* Candidate = SourcePin ? Cast<URigVMUnitNode>(SourcePin->GetNode()) : nullptr)
			{
				if (Candidate->GetScriptStruct() == FRigUnit_GetControlVector::StaticStruct())
				{
					URigVMPin* ControlPin = Candidate->FindPin(TEXT("Control"));
					if (ControlPin && ControlPin->GetDefaultValue().Equals(ControlName, ESearchCase::IgnoreCase))
					{
						GetterNode = Candidate;
						break;
					}
				}
			}
		}
		bool bCreatedGetter = false;
		if (!GetterNode)
		{
			if (!ExistingSources.IsEmpty())
			{
				Error = TEXT("ForceAndTorque.Linear is already linked to a different source; disconnect it explicitly before animation setup.");
				return false;
			}
			GetterNode = Controller->AddUnitNode(
				FRigUnit_GetControlVector::StaticStruct(), FRigUnit::GetMethodName(),
				ForceNode->GetPosition() - FVector2D(360.0, 0.0), FString(), true, false);
			if (!GetterNode
				|| !Controller->SetPinDefaultValue(GetterNode->GetName() + TEXT(".Control"), ControlName, true, true, false, false, true)
				|| !Controller->AddLink(GetterNode->GetName() + TEXT(".Vector"), ForceLinearPin->GetPinPath(), true, false))
			{
				Error = TEXT("Failed to create and connect Get Control Vector to ForceAndTorque.Linear.");
				return false;
			}
			bCreatedGetter = true;
		}

		// The hierarchy control and RigVM getter must exist in the generated class
		// before Sequencer creates its Control Rig instance. Otherwise UE silently
		// ignores position-key writes because the track instance cannot find the control.
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		UClass* RigClass = Rig->GeneratedClass;
		if (!RigClass || !RigClass->IsChildOf(UControlRig::StaticClass()))
		{
			Error = TEXT("The Control Rig Blueprint has no generated Control Rig class.");
			return false;
		}
		FMovieSceneBindingProxy Binding(BindingGuid, Sequence);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		UMovieSceneControlRigParameterTrack* Track = Cast<UMovieSceneControlRigParameterTrack>(
			UControlRigSequencerEditorLibrary::FindOrCreateControlRigTrack(World, Sequence, RigClass, Binding, false));
		if (!Track || !Track->GetControlRig())
		{
			Error = TEXT("Failed to find or create the bound Control Rig parameter track.");
			return false;
		}
		Track->RecreateControlRig();
		UControlRig* TrackRig = Track->GetControlRig();
		if (!TrackRig)
		{
			Error = TEXT("The Control Rig parameter track did not recreate its Control Rig instance.");
			return false;
		}
		TArray<FFrameNumber> Frames;
		TArray<FVector> Values;
		if (!ParseAnimationSamples(Args, Frames, Values, Error)) return false;
		if (!WritePositionControlKeys(Sequence, Track, ControlName, Frames, Values, Error)) return false;
		const int32 KeyCount = CountControlKeys(Track, ControlName);
		if (KeyCount < Frames.Num())
		{
			Error = FString::Printf(TEXT("Sequencer readback found %d key(s) for %s after requesting %d sample(s)."),
				KeyCount, *ControlName, Frames.Num());
			return false;
		}
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		if (!Context.Services.SaveAsset(SequencePath, false, Error)) return false;
		Out->SetStringField(TEXT("sequence_path"), SequencePath);
		Out->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Out->SetStringField(TEXT("track_path"), Track->GetPathName());
		Out->SetStringField(TEXT("control_name"), ControlName);
		Out->SetStringField(TEXT("getter_node"), GetterNode->GetName());
		Out->SetStringField(TEXT("force_pin"), ForceLinearPin->GetPinPath());
		Out->SetBoolField(TEXT("control_created"), bCreatedControl);
		Out->SetBoolField(TEXT("getter_created"), bCreatedGetter);
		Out->SetBoolField(TEXT("graph_link_verified"), ForceLinearPin->IsLinked(true));
		Out->SetNumberField(TEXT("requested_sample_count"), Frames.Num());
		Out->SetNumberField(TEXT("readback_key_count"), KeyCount);
		Summary = FString::Printf(TEXT("Bound %s to force %s and authored %d Sequencer position sample(s), with %d keys read back."),
			*ControlName, *NodeName, Frames.Num(), KeyCount);
		return true;
	}
	if (Name == TEXT("control_rig_physics_layer_configure"))
	{
		FString SequencePath, TrackName;
		ULevelSequence* Sequence = LoadSequence(Context, Args, SequencePath, Error);
		if (!Sequence) return false;
		Args->TryGetStringField(TEXT("track_name"), TrackName);
		UMovieSceneControlRigParameterTrack* Track = FindControlRigTrack(Sequence, Rig, TrackName);
		if (!Track || !Track->GetControlRig())
		{
			Error = TEXT("No matching Control Rig parameter track was found for the target rig and optional track_name.");
			return false;
		}
		bool bLayered = true;
		Args->TryGetBoolField(TEXT("layered"), bLayered);
		int32 Priority = 0;
		Args->TryGetNumberField(TEXT("priority_order"), Priority);
		const bool bBefore = UControlRigSequencerEditorLibrary::IsLayeredControlRig(Track->GetControlRig());
		bool bModeApplied = true;
		if (bBefore != bLayered)
		{
			bModeApplied = UControlRigSequencerEditorLibrary::SetControlRigLayeredMode(Track, bLayered);
		}
		Track->SetPriorityOrder(Priority);
		UControlRigSequencerEditorLibrary::MarkLayeredModeOnTrackDisplay(Track);
		Track->MarkPackageDirty();
		const bool bAfter = UControlRigSequencerEditorLibrary::IsLayeredControlRig(Track->GetControlRig());
		const int32 PriorityAfter = Track->GetPriorityOrder();
		if (!bModeApplied || bAfter != bLayered || PriorityAfter != Priority)
		{
			Error = TEXT("Layered Control Rig mode or priority readback did not match the requested values.");
			return false;
		}
		if (!Context.Services.SaveAsset(SequencePath, false, Error)) return false;
		Out->SetStringField(TEXT("sequence_path"), SequencePath);
		Out->SetStringField(TEXT("track_path"), Track->GetPathName());
		Out->SetBoolField(TEXT("layered_before"), bBefore);
		Out->SetBoolField(TEXT("layered"), bAfter);
		Out->SetNumberField(TEXT("priority_order"), PriorityAfter);
		Out->SetBoolField(TEXT("saved"), true);
		Summary = FString::Printf(TEXT("Configured Control Rig Physics track layered=%s with priority %d and verified readback."),
			bAfter ? TEXT("true") : TEXT("false"), PriorityAfter);
		return true;
	}
	if (Name == TEXT("control_rig_physics_force_remove"))
	{
		FString NodeName;
		if (!Args->TryGetStringField(TEXT("node_name"), NodeName) || NodeName.IsEmpty())
		{
			Error = TEXT("node_name is required.");
			return false;
		}
		URigVMUnitNode* Node = FindForceNode(Controller, NodeName, Error);
		if (!Node) return false;
		const FName ActualName = Node->GetFName();
		if (!Controller->RemoveNodeByName(ActualName, true, false))
		{
			Error = FString::Printf(TEXT("Failed to remove Control Rig Physics force node: %s"), *ActualName.ToString());
			return false;
		}
		Out->SetStringField(TEXT("removed_node"), ActualName.ToString());
		Out->SetBoolField(TEXT("readback_absent"), Controller->GetGraph()->FindNodeByName(ActualName) == nullptr);
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		Summary = FString::Printf(TEXT("Removed Control Rig Physics force node %s, compiled, saved, and verified absence."), *ActualName.ToString());
		return true;
	}
	if (Name == TEXT("control_rig_physics_compile_validate"))
	{
		int32 ForceNodeCount = 0;
		for (URigVMNode* Node : Controller->GetGraph()->GetNodes())
		{
			if (URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node))
			{
				if (UnitNode->GetScriptStruct() == FRigUnit_HierarchyAddPhysicsBodyForce::StaticStruct()) ++ForceNodeCount;
			}
		}
		Out->SetNumberField(TEXT("force_node_count"), ForceNodeCount);
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		Summary = FString::Printf(TEXT("Control Rig Physics graph compiled with %d Add Force node(s)."), ForceNodeCount);
		return true;
	}
	if (Name == TEXT("control_rig_physics_preview_receipt"))
	{
		int32 MaxWidth = 1280;
		int32 MaxHeight = 720;
		Args->TryGetNumberField(TEXT("max_width"), MaxWidth);
		Args->TryGetNumberField(TEXT("max_height"), MaxHeight);
		MaxWidth = FMath::Clamp(MaxWidth, 64, 3840);
		MaxHeight = FMath::Clamp(MaxHeight, 64, 2160);
		int32 ForceNodeCount = 0;
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (URigVMNode* Node : Controller->GetGraph()->GetNodes())
		{
			if (URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node))
			{
				if (UnitNode->GetScriptStruct() == FRigUnit_HierarchyAddPhysicsBodyForce::StaticStruct())
				{
					++ForceNodeCount;
					Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(UnitNode)));
				}
			}
		}
		if (ForceNodeCount == 0)
		{
			Error = TEXT("Preview receipt requires at least one real Control Rig Physics Add Force node.");
			return false;
		}
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		IConsoleVariable* Visualization = IConsoleManager::Get().FindConsoleVariable(TEXT("ControlRig.Physics.AllowVisualization"));
		if (!Visualization)
		{
			Error = TEXT("ControlRig.Physics.AllowVisualization console variable is unavailable.");
			return false;
		}
		Visualization->Set(1, ECVF_SetByConsole);
		if (Visualization->GetInt() == 0)
		{
			Error = TEXT("Control Rig Physics visualization did not enable.");
			return false;
		}
		if (!AddInlineScreenshotReceipt(Context, AssetPath, MaxWidth, MaxHeight, Out, Error)) return false;
		Out->SetNumberField(TEXT("force_node_count"), ForceNodeCount);
		Out->SetArrayField(TEXT("force_nodes"), Nodes);
		Out->SetBoolField(TEXT("visualization_enabled"), true);
		Out->SetBoolField(TEXT("graph_readback_ok"), true);
		Out->SetBoolField(TEXT("preview_capture_ok"), true);
		Out->SetStringField(TEXT("receipt_schema"), TEXT("somolmcp.control_rig_physics.preview_receipt.v1"));
		Out->SetStringField(TEXT("captured_at_utc"), FDateTime::UtcNow().ToIso8601());
		Summary = FString::Printf(TEXT("Compiled Control Rig Physics asset, verified %d force node(s), enabled visualization, and captured PNG preview evidence."), ForceNodeCount);
		return true;
	}

	Error = FString::Printf(TEXT("Unsupported UE 5.8 Control Rig Physics tool: %s"), *Name);
	return false;
}

static TSharedRef<FJsonObject> Schema()
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Control Rig Blueprint asset path."))},
		{TEXT("node_name"), FSololmcpSchemaBuilder::String(TEXT("Existing Add Force RigVM node name or path."))},
		{TEXT("force_name"), FSololmcpSchemaBuilder::String(TEXT("Optional force-record name written to the new node."))},
		{TEXT("control_name"), FSololmcpSchemaBuilder::String(TEXT("Position control created or reused to drive ForceAndTorque.Linear."))},
		{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("Level Sequence asset path for force animation or layer configuration."))},
		{TEXT("binding_guid"), FSololmcpSchemaBuilder::String(TEXT("Exact Level Sequence actor/component binding GUID used to create or find the Control Rig track."))},
		{TEXT("binding_name"), FSololmcpSchemaBuilder::String(TEXT("Exact, case-sensitive Level Sequence binding name. Accepted only when it uniquely identifies one binding; binding_guid takes precedence."))},
		{TEXT("track_name"), FSololmcpSchemaBuilder::String(TEXT("Optional Control Rig track name filter."))},
		{TEXT("layered"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable or disable native UE 5.8 Layered Control Rig mode."))},
		{TEXT("priority_order"), FSololmcpSchemaBuilder::Integer(TEXT("Control Rig track evaluation priority."))},
		{TEXT("samples"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
			{TEXT("frame"), FSololmcpSchemaBuilder::Integer(TEXT("Display-rate frame."))},
			{TEXT("x"), FSololmcpSchemaBuilder::Number(TEXT("Force vector X."))},
			{TEXT("y"), FSololmcpSchemaBuilder::Number(TEXT("Force vector Y."))},
			{TEXT("z"), FSololmcpSchemaBuilder::Number(TEXT("Force vector Z."))}
		}, {TEXT("frame"), TEXT("x"), TEXT("y"), TEXT("z")}), TEXT("Force-vector animation samples."))},
		{TEXT("max_width"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum preview PNG width."))},
		{TEXT("max_height"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum preview PNG height."))},
		{TEXT("position_x"), FSololmcpSchemaBuilder::Number(TEXT("Graph node X position."))},
		{TEXT("position_y"), FSololmcpSchemaBuilder::Number(TEXT("Graph node Y position."))},
		{TEXT("pin_values"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Map of node-relative pin paths to RigVM default-value strings."))}
	});
}
#endif
}

void RegisterUE58ControlRigPhysicsTools(FSololmcpToolRegistry& Registry)
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
	static const TCHAR* Names[] = {
		TEXT("control_rig_physics_capability_probe"),
		TEXT("control_rig_physics_force_add"),
		TEXT("control_rig_physics_force_update"),
		TEXT("control_rig_physics_force_remove"),
		TEXT("control_rig_physics_force_animate"),
		TEXT("control_rig_physics_layer_configure"),
		TEXT("control_rig_physics_compile_validate"),
		TEXT("control_rig_physics_preview_receipt")
	};
	for (const TCHAR* NamePtr : Names)
	{
		const FString Name(NamePtr);
		FSololmcpToolDefinition Def;
		Def.Name = Name;
		Def.Description = FString::Printf(TEXT("UE 5.8 Control Rig Physics graph transaction: %s"), *Name);
		Def.InputSchema = UE58ControlRigPhysics::Schema();
		Def.CacheTtlSeconds = Name.EndsWith(TEXT("_probe")) ? 2 : 0;
		Def.Execute = [Name](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return UE58ControlRigPhysics::Execute(Name, Context, Args, Out, Summary, Error);
		};
		Registry.Register(Def);
	}
#endif
}
}
#else
namespace UE::SOMOLMCP
{
void RegisterUE58ControlRigPhysicsTools(FSololmcpToolRegistry&)
{
}
}
#endif
