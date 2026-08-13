// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 Direct Mesh Control authoring and Sequencer bake tools.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#if SOMOLMCP_WITH_UE58_MESHPARTITION

#include "ControlRigBlueprintEditorLibrary.h"
#include "ControlRigBlueprintFactory.h"
#include "ControlRigBlueprintLegacy.h"
#include "ControlRigSequencerEditorLibrary.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelSequence.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneBindingProxy.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "Polygroups/GroupSetAdapter.h"
#include "Polygroups/PolygroupUtil.h"
#include "RigVMCore/RigVMStruct.h"
#include "RigVMEditorBlueprintLibrary.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMPin.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Sections/MovieSceneParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Units/Execution/RigUnit_PrepareForExecution.h"
#include "Units/RigUnit_DirectMeshControl.h"

namespace UE::SOMOLMCP
{
namespace UE58DirectMeshControl
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
using namespace UE::Geometry;

static bool InspectLayer(USkeletalMesh* Mesh, FName LayerName, TArray<FName>& Groups, FString& Error)
{
	if (!Mesh)
	{
		Error = TEXT("A valid skeletal mesh is required.");
		return false;
	}
	constexpr int32 LOD = 0;
	FMeshDescription* Description = Mesh->HasMeshDescription(LOD) ? Mesh->GetMeshDescription(LOD) : nullptr;
	if (!Description)
	{
		Error = TEXT("Direct Mesh Control requires an editable LOD0 MeshDescription.");
		return false;
	}
	FDynamicMesh3 DynamicMesh;
	FMeshDescriptionToDynamicMesh Converter;
	Converter.bTransformVertexColorsLinearToSRGB = false;
	Converter.bVIDsFromNonManifoldMeshDescriptionAttr = true;
	Converter.Convert(Description, DynamicMesh, true);
	const FDynamicMeshTriangleLabelAttribute* Attribute = FindTriangleLabelLayerByName(DynamicMesh, LayerName);
	if (!Attribute)
	{
		Error = FString::Printf(TEXT("Skeletal mesh %s has no Direct Mesh Control triangle label layer named '%s'. Create the layer with the UE 5.8 DMC Polygroup tool first."),
			*Mesh->GetPathName(), *LayerName.ToString());
		return false;
	}
	TSet<FName> Unique;
	for (const int32 TriangleId : DynamicMesh.TriangleIndicesItr())
	{
		const FName Value = Attribute->GetValue(TriangleId);
		if (!Value.IsNone()) Unique.Add(Value);
	}
	Groups = Unique.Array();
	Groups.Sort(FNameLexicalLess());
	if (Groups.IsEmpty())
	{
		Error = FString::Printf(TEXT("Direct Mesh Control layer '%s' contains no named triangle groups."), *LayerName.ToString());
		return false;
	}
	return true;
}

static UControlRigBlueprint* LoadRig(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
	FString& AssetPath, FString& Error)
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

static bool CompileAndSave(const FSololmcpToolExecutionContext& Context, UControlRigBlueprint* Rig,
	const FString& AssetPath, TSharedRef<FJsonObject>& Out, FString& Error)
{
	FCompilerResultsLog Log;
	FKismetEditorUtilities::CompileBlueprint(Rig, EBlueprintCompileOptions::None, &Log);
	const bool bCompiled = Rig->Status != BS_Error && Log.NumErrors == 0;
	Out->SetBoolField(TEXT("compiled"), bCompiled);
	Out->SetNumberField(TEXT("compile_errors"), Log.NumErrors);
	Out->SetNumberField(TEXT("compile_warnings"), Log.NumWarnings);
	if (!bCompiled)
	{
		Error = FString::Printf(TEXT("Direct Mesh Control rig compile failed with %d error(s)."), Log.NumErrors);
		return false;
	}
	if (!Context.Services.SaveAsset(AssetPath, false, Error)) return false;
	Out->SetBoolField(TEXT("saved"), true);
	return true;
}

static URigVMController* GetConstructionController(UControlRigBlueprint* Rig, URigVMUnitNode*& EventNode, FString& Error)
{
	EventNode = nullptr;
	const URigVMGraph* ConstructionGraph = nullptr;
	FRigVMClient* RigClient = Rig->IControlRigEditorAssetInterface::GetRigVMClient();
	for (URigVMGraph* Graph : RigClient->GetAllModels(false, false))
	{
		for (URigVMNode* Node : Graph->GetNodes())
		{
			if (Node && Node->IsEvent() && Node->GetEventName() == FRigUnit_PrepareForExecution::EventName)
			{
				EventNode = Cast<URigVMUnitNode>(Node);
				ConstructionGraph = Graph;
				break;
			}
		}
		if (EventNode) break;
	}
	if (!ConstructionGraph)
	{
		ConstructionGraph = RigClient->AddModel(TEXT("ConstructionGraph"), true);
		if (!ConstructionGraph)
		{
			Error = TEXT("Failed to create the Control Rig ConstructionGraph.");
			return nullptr;
		}
		URigVMController* NewController = RigClient->GetOrCreateController(ConstructionGraph);
		EventNode = NewController ? NewController->AddUnitNode(FRigUnit_PrepareForExecution::StaticStruct(),
			FRigUnit::GetMethodName(), FVector2D::ZeroVector, FString(), true, false) : nullptr;
		if (!EventNode)
		{
			Error = TEXT("Failed to create the Control Rig Construction Event node.");
			return nullptr;
		}
	}
	URigVMController* Controller = RigClient->GetOrCreateController(ConstructionGraph);
	if (!Controller) Error = TEXT("ConstructionGraph controller is unavailable.");
	return Controller;
}

static URigVMUnitNode* FindSetupNode(UControlRigBlueprint* Rig, FName LayerName)
{
	FRigVMClient* RigClient = Rig->IControlRigEditorAssetInterface::GetRigVMClient();
	for (URigVMGraph* Graph : RigClient->GetAllModels(false, false))
	{
		for (URigVMNode* Node : Graph->GetNodes())
		{
			if (URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node))
			{
				if (Unit->GetScriptStruct() == FRigUnit_SetupShapeLibraryFromLayer::StaticStruct())
				{
					const URigVMPin* Pin = Unit->FindPin(TEXT("LayerName"));
					if (Pin && FName(*Pin->GetDefaultValue()) == LayerName) return Unit;
				}
			}
		}
	}
	return nullptr;
}

static bool EnsureSetupNode(UControlRigBlueprint* Rig, FName LayerName, URigVMUnitNode*& SetupNode, FString& Error)
{
	SetupNode = FindSetupNode(Rig, LayerName);
	if (SetupNode) return true;
	URigVMUnitNode* EventNode = nullptr;
	URigVMController* Controller = GetConstructionController(Rig, EventNode, Error);
	if (!Controller) return false;
	SetupNode = Controller->AddUnitNode(FRigUnit_SetupShapeLibraryFromLayer::StaticStruct(), FRigUnit::GetMethodName(),
		FVector2D(420.0, 0.0), FString(), true, false);
	if (!SetupNode
		|| !Controller->SetPinDefaultValue(SetupNode->GetName() + TEXT(".LayerName"), LayerName.ToString(), true, true, false, false, true))
	{
		Error = TEXT("Failed to add or configure Set Shape Library from Layer in ConstructionGraph.");
		return false;
	}
	URigVMPin* Source = EventNode ? EventNode->FindExecutePin() : nullptr;
	URigVMPin* Target = SetupNode->FindPin(FRigVMStruct::ExecuteContextName.ToString());
	if (!Source || !Target || !Controller->AddLink(Source->GetPinPath(), Target->GetPinPath(), true, false))
	{
		Error = TEXT("Failed to link Construction Event to Set Shape Library from Layer.");
		return false;
	}
	return true;
}

static bool ParseTransform(const TSharedRef<FJsonObject>& Args, FTransform& Transform, FString& Error)
{
	double X = 0, Y = 0, Z = 0, Pitch = 0, Yaw = 0, Roll = 0, SX = 1, SY = 1, SZ = 1;
	Args->TryGetNumberField(TEXT("x"), X); Args->TryGetNumberField(TEXT("y"), Y); Args->TryGetNumberField(TEXT("z"), Z);
	Args->TryGetNumberField(TEXT("pitch"), Pitch); Args->TryGetNumberField(TEXT("yaw"), Yaw); Args->TryGetNumberField(TEXT("roll"), Roll);
	Args->TryGetNumberField(TEXT("scale_x"), SX); Args->TryGetNumberField(TEXT("scale_y"), SY); Args->TryGetNumberField(TEXT("scale_z"), SZ);
	if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z) || !FMath::IsFinite(Pitch)
		|| !FMath::IsFinite(Yaw) || !FMath::IsFinite(Roll) || !FMath::IsFinite(SX) || !FMath::IsFinite(SY) || !FMath::IsFinite(SZ)
		|| FMath::IsNearlyZero(SX) || FMath::IsNearlyZero(SY) || FMath::IsNearlyZero(SZ))
	{
		Error = TEXT("Transform values must be finite and scale components must be non-zero.");
		return false;
	}
	Transform = FTransform(FRotator(Pitch, Yaw, Roll), FVector(X, Y, Z), FVector(SX, SY, SZ));
	return true;
}

static UMovieSceneControlRigParameterTrack* FindRigTrack(ULevelSequence* Sequence, UControlRigBlueprint* Rig)
{
	for (const FControlRigSequencerBindingProxy& Proxy : UControlRigSequencerEditorLibrary::GetControlRigs(Sequence))
	{
		if (Proxy.Track && Proxy.ControlRig && Proxy.ControlRig->GetClass()
			&& Proxy.ControlRig->GetClass()->ClassGeneratedBy == Rig) return Proxy.Track;
	}
	return nullptr;
}

static int32 CountTransformKeys(UMovieSceneControlRigParameterTrack* Track, FName ControlName)
{
	if (!Track) return 0;
	for (UMovieSceneSection* Section : Track->GetAllSections())
	{
		if (UMovieSceneControlRigParameterSection* RigSection = Cast<UMovieSceneControlRigParameterSection>(Section))
		{
			for (const FTransformParameterNameAndCurves& Curves : RigSection->GetTransformParameterNamesAndCurves())
			{
				if (Curves.ParameterName != ControlName) continue;
				int32 Count = 0;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					Count += Curves.Translation[Axis].GetNumKeys();
					Count += Curves.Rotation[Axis].GetNumKeys();
					Count += Curves.Scale[Axis].GetNumKeys();
				}
				return Count;
			}
		}
	}
	return 0;
}

static bool WriteTransformControlKeys(
	ULevelSequence* Sequence,
	UMovieSceneControlRigParameterTrack* Track,
	FName ControlName,
	const TArray<FFrameNumber>& DisplayFrames,
	const TArray<FTransform>& Values,
	FString& Error)
{
	if (!Sequence || !Sequence->GetMovieScene() || !Track || DisplayFrames.Num() != Values.Num())
	{
		Error = TEXT("Invalid Level Sequence, Control Rig track, or transform sample arrays.");
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
	if (!RigSection->HasTransformParameter(ControlName))
	{
		RigSection->AddTransformParameter(ControlName, FEulerTransform(FTransform::Identity), true);
	}
	FTransformParameterNameAndCurves* Curves = nullptr;
	for (FTransformParameterNameAndCurves& Candidate : RigSection->GetTransformParameterNamesAndCurves())
	{
		if (Candidate.ParameterName == ControlName)
		{
			Curves = &Candidate;
			break;
		}
	}
	if (!Curves)
	{
		Error = FString::Printf(TEXT("Control Rig section did not expose transform channels for %s."), *ControlName.ToString());
		return false;
	}
	const FFrameRate TickResolution = Sequence->GetMovieScene()->GetTickResolution();
	const FFrameRate DisplayRate = Sequence->GetMovieScene()->GetDisplayRate();
	for (int32 Index = 0; Index < DisplayFrames.Num(); ++Index)
	{
		const FFrameNumber TickFrame = FFrameRate::TransformTime(
			FFrameTime(DisplayFrames[Index], 0), DisplayRate, TickResolution).RoundToFrame();
		const FVector Translation = Values[Index].GetTranslation();
		const FRotator Rotation = Values[Index].Rotator();
		const FVector Scale = Values[Index].GetScale3D();
		const double TranslationValues[3] = { Translation.X, Translation.Y, Translation.Z };
		const double RotationValues[3] = { Rotation.Roll, Rotation.Pitch, Rotation.Yaw };
		const double ScaleValues[3] = { Scale.X, Scale.Y, Scale.Z };
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Curves->Translation[Axis].GetData().UpdateOrAddKey(TickFrame, FMovieSceneFloatValue(TranslationValues[Axis]));
			Curves->Rotation[Axis].GetData().UpdateOrAddKey(TickFrame, FMovieSceneFloatValue(RotationValues[Axis]));
			Curves->Scale[Axis].GetData().UpdateOrAddKey(TickFrame, FMovieSceneFloatValue(ScaleValues[Axis]));
		}
		RigSection->ExpandToFrame(TickFrame);
	}
	RigSection->ReconstructChannelProxy();
	RigSection->MarkPackageDirty();
	Track->MarkPackageDirty();
	return true;
}

static bool Execute(const FString& Name, const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	FString AssetPath;
	UControlRigBlueprint* Rig = nullptr;
	if (Name == TEXT("direct_mesh_control_create"))
	{
		FString MeshPath, LayerString = TEXT("dmc-polygroup");
		if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty()
			|| !Args->TryGetStringField(TEXT("skeletal_mesh_path"), MeshPath) || MeshPath.IsEmpty())
		{
			Error = TEXT("asset_path and skeletal_mesh_path are required.");
			return false;
		}
		Args->TryGetStringField(TEXT("layer_name"), LayerString);
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(MeshPath, Error));
		if (!Mesh) return false;
		TArray<FName> Groups;
		if (!InspectLayer(Mesh, FName(*LayerString), Groups, Error)) return false;
		Rig = UControlRigBlueprintFactory::CreateNewControlRigAsset(AssetPath, false);
		if (!Rig)
		{
			Error = FString::Printf(TEXT("Failed to create Direct Mesh Control rig: %s"), *AssetPath);
			return false;
		}
		UControlRigBlueprintEditorLibrary::SetPreviewMesh(Rig, Mesh, true);
		URigVMUnitNode* SetupNode = nullptr;
		if (!EnsureSetupNode(Rig, FName(*LayerString), SetupNode, Error)) return false;
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetStringField(TEXT("skeletal_mesh_path"), MeshPath);
		Out->SetStringField(TEXT("layer_name"), LayerString);
		Out->SetStringField(TEXT("setup_node"), SetupNode->GetNodePath());
		TArray<TSharedPtr<FJsonValue>> GroupJson;
		for (FName Group : Groups) GroupJson.Add(MakeShared<FJsonValueString>(Group.ToString()));
		Out->SetArrayField(TEXT("groups"), GroupJson);
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		Summary = FString::Printf(TEXT("Created Direct Mesh Control rig with %d validated group(s) and a linked Construction Event setup node."), Groups.Num());
		return true;
	}

	Rig = LoadRig(Context, Args, AssetPath, Error);
	if (!Rig) return false;
	USkeletalMesh* Mesh = Rig->GetPreviewMesh();
	if (!Mesh)
	{
		Error = TEXT("The Direct Mesh Control rig has no preview skeletal mesh.");
		return false;
	}
	FString LayerString = TEXT("dmc-polygroup");
	Args->TryGetStringField(TEXT("layer_name"), LayerString);
	TArray<FName> Groups;
	if (!InspectLayer(Mesh, FName(*LayerString), Groups, Error)) return false;
	URigVMUnitNode* SetupNode = FindSetupNode(Rig, FName(*LayerString));
	if (!SetupNode)
	{
		Error = TEXT("The rig has no matching Set Shape Library from Layer node in its ConstructionGraph.");
		return false;
	}
	Out->SetStringField(TEXT("asset_path"), AssetPath);
	Out->SetStringField(TEXT("skeletal_mesh_path"), Mesh->GetPathName());
	Out->SetStringField(TEXT("layer_name"), LayerString);

	if (Name == TEXT("direct_mesh_control_element_bind"))
	{
		FString GroupName, ControlName;
		if (!Args->TryGetStringField(TEXT("group_name"), GroupName) || GroupName.IsEmpty())
		{
			Error = TEXT("group_name is required."); return false;
		}
		Args->TryGetStringField(TEXT("control_name"), ControlName);
		if (ControlName.IsEmpty()) ControlName = GroupName + TEXT("_CTRL");
		if (!Groups.Contains(FName(*GroupName)))
		{
			Error = FString::Printf(TEXT("group_name '%s' is not present in Direct Mesh Control layer '%s'."), *GroupName, *LayerString);
			return false;
		}
		URigHierarchyController* Controller = UControlRigBlueprintEditorLibrary::GetHierarchyController(Rig);
		URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(Rig);
		if (!Controller || !Hierarchy) { Error = TEXT("Control Rig hierarchy is unavailable."); return false; }
		const FRigElementKey Key(FName(*ControlName), ERigElementType::Control);
		if (Hierarchy->Find(Key))
		{
			Error = FString::Printf(TEXT("Control already exists; refusing ambiguous rebind: %s"), *ControlName);
			return false;
		}
		FRigControlSettings Settings;
		Settings.ControlType = ERigControlType::Transform;
		Settings.DisplayName = FName(*ControlName);
		Settings.ShapeName = FName(*GroupName);
		const FRigElementKey Created = Controller->AddControl(FName(*ControlName), FRigElementKey(), Settings,
			FRigControlValue::Make(FTransform::Identity), FTransform::Identity, FTransform::Identity, true, false);
		if (!Created.IsValid()) { Error = TEXT("Failed to create the Direct Mesh Control transform control."); return false; }
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		const FRigControlSettings Readback = Controller->GetControlSettings(Created);
		if (Readback.ShapeName != FName(*GroupName) || Readback.ControlType != ERigControlType::Transform)
		{
			Error = TEXT("Direct Mesh Control binding readback did not match the requested group and transform control type.");
			return false;
		}
		Out->SetStringField(TEXT("group_name"), GroupName);
		Out->SetStringField(TEXT("control_name"), ControlName);
		Out->SetStringField(TEXT("shape_name_readback"), Readback.ShapeName.ToString());
		Summary = FString::Printf(TEXT("Bound DMC group %s to transform control %s and verified the shape binding."), *GroupName, *ControlName);
		return true;
	}

	if (Name == TEXT("direct_mesh_control_transform_set"))
	{
		FString ControlName;
		if (!Args->TryGetStringField(TEXT("control_name"), ControlName) || ControlName.IsEmpty()) { Error = TEXT("control_name is required."); return false; }
		URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(Rig);
		const FRigElementKey Key(FName(*ControlName), ERigElementType::Control);
		if (!Hierarchy || !Hierarchy->Find(Key)) { Error = FString::Printf(TEXT("Transform control was not found: %s"), *ControlName); return false; }
		FTransform Value;
		if (!ParseTransform(Args, Value, Error)) return false;
		const int32 ControlIndex = Hierarchy->GetIndex(Key);
		const FRigControlValue RigValue = FRigControlValue::Make(Value);
		Hierarchy->SetControlValueByIndex(ControlIndex, RigValue, ERigControlValueType::Initial, true, false);
		Hierarchy->SetControlValueByIndex(ControlIndex, RigValue, ERigControlValueType::Current, true, false);
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		const FTransform Readback = Hierarchy->GetControlValue(Key, ERigControlValueType::Current)
			.Get<FRigControlValue::FTransform_Float>().ToTransform();
		Out->SetStringField(TEXT("control_name"), ControlName);
		Out->SetStringField(TEXT("transform_readback"), Readback.ToHumanReadableString());
		Summary = FString::Printf(TEXT("Set, compiled, saved, and read back transform control %s."), *ControlName);
		return true;
	}

	if (Name == TEXT("direct_mesh_control_bake_to_sequence"))
	{
		FString SequencePath, ControlName, BindingGuidText, BindingName;
		if (!Args->TryGetStringField(TEXT("sequence_path"), SequencePath) || SequencePath.IsEmpty()
			|| !Args->TryGetStringField(TEXT("control_name"), ControlName) || ControlName.IsEmpty())
		{
			Error = TEXT("sequence_path and control_name are required."); return false;
		}
		ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(SequencePath, Error));
		if (!Sequence || !Sequence->GetMovieScene()) return false;
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		FGuid BindingGuid;
		Args->TryGetStringField(TEXT("binding_guid"), BindingGuidText);
		Args->TryGetStringField(TEXT("binding_name"), BindingName);
		if (!BindingGuidText.IsEmpty())
		{
			if (!FGuid::Parse(BindingGuidText, BindingGuid) || !MovieScene->FindBinding(BindingGuid)) { Error = TEXT("binding_guid is invalid or absent from the Level Sequence."); return false; }
		}
		else if (!BindingName.IsEmpty())
		{
			int32 Matches = 0;
			const UMovieScene* ConstMovieScene = MovieScene;
			for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
			{
				FString Candidate;
				if (const FMovieScenePossessable* P = MovieScene->FindPossessable(Binding.GetObjectGuid())) Candidate = P->GetName();
				else if (const FMovieSceneSpawnable* S = MovieScene->FindSpawnable(Binding.GetObjectGuid())) Candidate = S->GetName();
				if (Candidate.Equals(BindingName, ESearchCase::CaseSensitive)) { BindingGuid = Binding.GetObjectGuid(); ++Matches; }
			}
			if (Matches != 1) { Error = FString::Printf(TEXT("binding_name must match exactly one sequence binding; found %d."), Matches); return false; }
		}
		else { Error = TEXT("binding_guid or an exact unique binding_name is required."); return false; }
		const FRigElementKey ControlKey(FName(*ControlName), ERigElementType::Control);
		URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(Rig);
		if (!Hierarchy || !Hierarchy->Find(ControlKey)) { Error = TEXT("The requested Direct Mesh Control transform control does not exist."); return false; }
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		UClass* RigClass = Rig->GeneratedClass;
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		UMovieSceneControlRigParameterTrack* Track = FindRigTrack(Sequence, Rig);
		if (!Track)
		{
			Track = Cast<UMovieSceneControlRigParameterTrack>(UControlRigSequencerEditorLibrary::FindOrCreateControlRigTrack(
				World, Sequence, RigClass, FMovieSceneBindingProxy(BindingGuid, Sequence), false));
		}
		if (!Track || !Track->GetControlRig()) { Error = TEXT("Failed to create the bound Control Rig parameter track."); return false; }
		Track->RecreateControlRig();
		UControlRig* TrackRig = Track->GetControlRig();
		const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
		if (!Args->TryGetArrayField(TEXT("samples"), Samples) || !Samples || Samples->IsEmpty()) { Error = TEXT("samples must contain frame and transform objects."); return false; }
		TArray<FFrameNumber> Frames;
		TArray<FTransform> Values;
		for (int32 Index = 0; Index < Samples->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Sample = nullptr;
			if (!(*Samples)[Index].IsValid() || !(*Samples)[Index]->TryGetObject(Sample) || !Sample || !Sample->IsValid()) { Error = TEXT("Each sample must be an object."); return false; }
			double Frame = 0;
			if (!(*Sample)->TryGetNumberField(TEXT("frame"), Frame)) { Error = TEXT("Each sample requires numeric frame."); return false; }
			FTransform Transform;
			if (!ParseTransform((*Sample).ToSharedRef(), Transform, Error)) return false;
			Frames.Add(FFrameNumber(FMath::RoundToInt32(Frame)));
			Values.Add(Transform);
		}
		if (!WriteTransformControlKeys(Sequence, Track, FName(*ControlName), Frames, Values, Error)) return false;
		const int32 KeyCount = CountTransformKeys(Track, FName(*ControlName));
		if (KeyCount < Frames.Num() * 9) { Error = FString::Printf(TEXT("Transform bake readback found %d channel keys; expected at least %d."), KeyCount, Frames.Num() * 9); return false; }
		if (!Context.Services.SaveAsset(SequencePath, false, Error)) return false;
		Out->SetStringField(TEXT("sequence_path"), SequencePath);
		Out->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Out->SetStringField(TEXT("control_name"), ControlName);
		Out->SetNumberField(TEXT("sample_count"), Frames.Num());
		Out->SetNumberField(TEXT("readback_channel_key_count"), KeyCount);
		Summary = FString::Printf(TEXT("Baked %d Direct Mesh Control transform sample(s) with %d channel keys read back."), Frames.Num(), KeyCount);
		return true;
	}

	if (Name == TEXT("direct_mesh_control_compile_validate"))
	{
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		URigVMGraph* Graph = SetupNode->GetGraph();
		URigVMPin* ExecutePin = SetupNode->FindPin(FRigVMStruct::ExecuteContextName.ToString());
		const bool bLinked = ExecutePin && ExecutePin->IsLinked(true);
		if (!Graph || !bLinked) { Error = TEXT("Direct Mesh Control setup node is not linked to the Construction Event execution chain."); return false; }
		int32 BoundControls = 0;
		URigHierarchyController* Controller = UControlRigBlueprintEditorLibrary::GetHierarchyController(Rig);
		URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(Rig);
		for (const FRigElementKey& Key : Hierarchy->GetControlKeys())
		{
			const FRigControlSettings Settings = Controller->GetControlSettings(Key);
			if (Groups.Contains(Settings.ShapeName)) ++BoundControls;
		}
		Out->SetNumberField(TEXT("group_count"), Groups.Num());
		Out->SetNumberField(TEXT("bound_control_count"), BoundControls);
		Out->SetBoolField(TEXT("construction_link_verified"), true);
		Out->SetStringField(TEXT("setup_node"), SetupNode->GetNodePath());
		Summary = FString::Printf(TEXT("Validated Direct Mesh Control rig: %d source groups, %d bound controls, Construction Event linked, compiled and saved."), Groups.Num(), BoundControls);
		return true;
	}

	Error = FString::Printf(TEXT("Unsupported UE 5.8 Direct Mesh Control tool: %s"), *Name);
	return false;
}

static TSharedRef<FJsonObject> Schema()
{
	const TSharedRef<FJsonObject> Sample = FSololmcpSchemaBuilder::Object({
		{TEXT("frame"), FSololmcpSchemaBuilder::Number()}, {TEXT("x"), FSololmcpSchemaBuilder::Number()},
		{TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()},
		{TEXT("pitch"), FSololmcpSchemaBuilder::Number()}, {TEXT("yaw"), FSololmcpSchemaBuilder::Number()},
		{TEXT("roll"), FSololmcpSchemaBuilder::Number()}, {TEXT("scale_x"), FSololmcpSchemaBuilder::Number()},
		{TEXT("scale_y"), FSololmcpSchemaBuilder::Number()}, {TEXT("scale_z"), FSololmcpSchemaBuilder::Number()}
	}, {TEXT("frame")});
	return FSololmcpSchemaBuilder::Object({
		{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Control Rig asset path; create uses this as the new asset path."))},
		{TEXT("skeletal_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Skeletal mesh containing the Direct Mesh Control triangle label layer."))},
		{TEXT("layer_name"), FSololmcpSchemaBuilder::String(TEXT("DMC triangle label layer; defaults to dmc-polygroup."))},
		{TEXT("group_name"), FSololmcpSchemaBuilder::String(TEXT("Exact DMC group/shape name."))},
		{TEXT("control_name"), FSololmcpSchemaBuilder::String(TEXT("Exact transform control name."))},
		{TEXT("sequence_path"), FSololmcpSchemaBuilder::String()}, {TEXT("binding_guid"), FSololmcpSchemaBuilder::String()},
		{TEXT("binding_name"), FSololmcpSchemaBuilder::String()}, {TEXT("samples"), FSololmcpSchemaBuilder::Array(Sample)},
		{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()},
		{TEXT("pitch"), FSololmcpSchemaBuilder::Number()}, {TEXT("yaw"), FSololmcpSchemaBuilder::Number()}, {TEXT("roll"), FSololmcpSchemaBuilder::Number()},
		{TEXT("scale_x"), FSololmcpSchemaBuilder::Number()}, {TEXT("scale_y"), FSololmcpSchemaBuilder::Number()}, {TEXT("scale_z"), FSololmcpSchemaBuilder::Number()}
	});
}
#endif
}

void RegisterUE58DirectMeshControlTools(FSololmcpToolRegistry& Registry)
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
	static const TCHAR* Names[] = {
		TEXT("direct_mesh_control_create"), TEXT("direct_mesh_control_element_bind"),
		TEXT("direct_mesh_control_transform_set"), TEXT("direct_mesh_control_bake_to_sequence"),
		TEXT("direct_mesh_control_compile_validate")
	};
	for (const TCHAR* NamePtr : Names)
	{
		const FString Name(NamePtr);
		FSololmcpToolDefinition Def;
		Def.Name = Name;
		Def.Description = FString::Printf(TEXT("UE 5.8 Direct Mesh Control fail-closed authoring transaction: %s"), *Name);
		Def.InputSchema = UE58DirectMeshControl::Schema();
		Def.CacheTtlSeconds = 0;
		Def.Execute = [Name](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return UE58DirectMeshControl::Execute(Name, Context, Args, Out, Summary, Error);
		};
		Registry.Register(Def);
	}
#endif
}
}
#else
namespace UE::SOMOLMCP
{
void RegisterUE58DirectMeshControlTools(FSololmcpToolRegistry&)
{
}
}
#endif
