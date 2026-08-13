// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 modular Control Rig authoring and validation tools.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#if SOMOLMCP_WITH_UE58_MESHPARTITION

#include "AssetRegistry/AssetRegistryModule.h"
#include "ControlRigAssetReference.h"
#include "ControlRigBlueprintEditorLibrary.h"
#include "ControlRigBlueprintFactory.h"
#include "Dom/JsonObject.h"
#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MeshBoneReduction.h"
#include "ModularRigController.h"
#include "ModularRigModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rigs/RigModuleDefines.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::SOMOLMCP
{
namespace UE58CharacterAnimation
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
static UControlRigBlueprint* LoadModularRig(
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
	if (!Rig)
	{
		if (Error.IsEmpty()) Error = FString::Printf(TEXT("Control Rig asset was not found: %s"), *AssetPath);
		return nullptr;
	}
	if (!Rig->IsModularRig())
	{
		Error = FString::Printf(TEXT("Control Rig is not a modular rig: %s"), *AssetPath);
		return nullptr;
	}
	return Rig;
}

static USkeletalMesh* LoadSkeletalMesh(
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
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(AssetPath, Error));
	if (!Mesh && Error.IsEmpty()) Error = FString::Printf(TEXT("Skeletal Mesh was not found: %s"), *AssetPath);
	return Mesh;
}

static UMorphTarget* ResolveMorphTarget(USkeletalMesh* Mesh, const TSharedRef<FJsonObject>& Args, FString& Error)
{
	FString MorphName;
	if (!Args->TryGetStringField(TEXT("morph_target"), MorphName) || MorphName.IsEmpty())
	{
		Error = TEXT("morph_target is required.");
		return nullptr;
	}
	UMorphTarget* Morph = Mesh ? Mesh->FindMorphTarget(FName(*MorphName)) : nullptr;
	if (!Morph) Error = FString::Printf(TEXT("Morph target was not found: %s"), *MorphName);
	return Morph;
}

static bool ResolveMorphLod(USkeletalMesh* Mesh, UMorphTarget* Morph, const TSharedRef<FJsonObject>& Args, int32& LodIndex, FString& Error)
{
	LodIndex = 0;
	Args->TryGetNumberField(TEXT("lod_index"), LodIndex);
	const FSkeletalMeshModel* ImportedModel = Mesh ? Mesh->GetImportedModel() : nullptr;
	if (!ImportedModel || !ImportedModel->LODModels.IsValidIndex(LodIndex) || !Morph->GetMorphLODModels().IsValidIndex(LodIndex))
	{
		Error = FString::Printf(TEXT("LOD %d is unavailable for the Skeletal Mesh or morph target."), LodIndex);
		return false;
	}
	return true;
}

static TSharedRef<FJsonObject> DeltaToJson(const FMorphTargetDelta& Delta, int32 DeltaIndex)
{
	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetNumberField(TEXT("delta_index"), DeltaIndex);
	Row->SetNumberField(TEXT("source_index"), Delta.SourceIdx);
	Row->SetArrayField(TEXT("position_delta"), {
		MakeShared<FJsonValueNumber>(Delta.PositionDelta.X),
		MakeShared<FJsonValueNumber>(Delta.PositionDelta.Y),
		MakeShared<FJsonValueNumber>(Delta.PositionDelta.Z)});
	Row->SetArrayField(TEXT("tangent_z_delta"), {
		MakeShared<FJsonValueNumber>(Delta.TangentZDelta.X),
		MakeShared<FJsonValueNumber>(Delta.TangentZDelta.Y),
		MakeShared<FJsonValueNumber>(Delta.TangentZDelta.Z)});
	return Row;
}

static FString BlendshapeSnapshotDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("BlendshapeSnapshots"));
}

static bool WriteMorphSnapshot(USkeletalMesh* Mesh, UMorphTarget* Morph, int32 LodIndex, FString& SnapshotId, FString& SnapshotPath, FString& Error)
{
	const TConstArrayView<FMorphTargetDelta> Deltas = Morph->GetMorphTargetDeltas(LodIndex);
	if (Deltas.Num() > 500000)
	{
		Error = TEXT("Morph snapshot exceeds the 500000-delta safety limit.");
		return false;
	}
	SnapshotId = FString::Printf(TEXT("morph_%s_%s"), *Morph->GetName(), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S_%s")));
	SnapshotId.ReplaceInline(TEXT("."), TEXT("_"));
	const FString Directory = BlendshapeSnapshotDirectory();
	IFileManager::Get().MakeDirectory(*Directory, true);
	SnapshotPath = FPaths::Combine(Directory, SnapshotId + TEXT(".json"));
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
	Root->SetStringField(TEXT("morph_target"), Morph->GetName());
	Root->SetNumberField(TEXT("lod_index"), LodIndex);
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Deltas.Num());
	for (int32 Index = 0; Index < Deltas.Num(); ++Index) Rows.Add(MakeShared<FJsonValueObject>(DeltaToJson(Deltas[Index], Index)));
	Root->SetArrayField(TEXT("deltas"), Rows);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer) || !FFileHelper::SaveStringToFile(Json, *SnapshotPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Error = FString::Printf(TEXT("Failed to write morph snapshot: %s"), *SnapshotPath);
		return false;
	}
	return true;
}

static bool ReadVector3f(const TSharedPtr<FJsonObject>& Row, const TCHAR* Field, FVector3f& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Row.IsValid() || !Row->TryGetArrayField(Field, Values) || !Values || Values->Num() != 3) return false;
	Out = FVector3f((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
	return true;
}

static bool ExecuteBlendshape(
	const FString& Name,
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Args,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FString AssetPath;
	USkeletalMesh* Mesh = LoadSkeletalMesh(Context, Args, AssetPath, Error);
	if (!Mesh) return false;
	UMorphTarget* Morph = ResolveMorphTarget(Mesh, Args, Error);
	if (!Morph) return false;
	int32 LodIndex = 0;
	if (!ResolveMorphLod(Mesh, Morph, Args, LodIndex, Error)) return false;
	const TConstArrayView<FMorphTargetDelta> CurrentDeltas = Morph->GetMorphTargetDeltas(LodIndex);
	Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
	Out->SetStringField(TEXT("morph_target"), Morph->GetName());
	Out->SetNumberField(TEXT("lod_index"), LodIndex);
	Out->SetNumberField(TEXT("delta_count"), CurrentDeltas.Num());

	if (Name == TEXT("skeletal_blendshape_mesh_element_inspect"))
	{
		int32 DeltaIndex = 0;
		Args->TryGetNumberField(TEXT("delta_index"), DeltaIndex);
		if (!CurrentDeltas.IsValidIndex(DeltaIndex))
		{
			Error = FString::Printf(TEXT("delta_index %d is outside [0, %d)."), DeltaIndex, CurrentDeltas.Num());
			return false;
		}
		Out->SetObjectField(TEXT("element"), DeltaToJson(CurrentDeltas[DeltaIndex], DeltaIndex));
		Summary = TEXT("Inspected a UE 5.8 Skeletal Mesh blendshape delta element.");
		return true;
	}
	if (Name == TEXT("skeletal_blendshape_edit_snapshot"))
	{
		FString SnapshotId, SnapshotPath;
		if (!WriteMorphSnapshot(Mesh, Morph, LodIndex, SnapshotId, SnapshotPath, Error)) return false;
		Out->SetStringField(TEXT("snapshot_id"), SnapshotId);
		Out->SetStringField(TEXT("snapshot_path"), SnapshotPath);
		Summary = TEXT("Persisted a project-local UE 5.8 blendshape edit snapshot.");
		return true;
	}
	if (Name == TEXT("skeletal_blendshape_mesh_element_move"))
	{
		int32 DeltaIndex = 0;
		Args->TryGetNumberField(TEXT("delta_index"), DeltaIndex);
		if (!CurrentDeltas.IsValidIndex(DeltaIndex))
		{
			Error = FString::Printf(TEXT("delta_index %d is outside [0, %d)."), DeltaIndex, CurrentDeltas.Num());
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* OffsetValues = nullptr;
		if (!Args->TryGetArrayField(TEXT("offset"), OffsetValues) || !OffsetValues || OffsetValues->Num() != 3)
		{
			Error = TEXT("offset must contain exactly three numbers.");
			return false;
		}
		FString SnapshotId, SnapshotPath;
		if (!WriteMorphSnapshot(Mesh, Morph, LodIndex, SnapshotId, SnapshotPath, Error)) return false;
		TArray<FMorphTargetDelta> Deltas(CurrentDeltas);
		Deltas[DeltaIndex].PositionDelta += FVector3f((*OffsetValues)[0]->AsNumber(), (*OffsetValues)[1]->AsNumber(), (*OffsetValues)[2]->AsNumber());
		const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
		Morph->Modify();
		Mesh->Modify();
		Morph->PopulateDeltas(Deltas, LodIndex, ImportedModel->LODModels[LodIndex].Sections, true, false);
		Mesh->InitMorphTargetsAndRebuildRenderData();
		Mesh->MarkPackageDirty();
		if (!Context.Services.SaveAsset(Mesh->GetPathName(), false, Error)) return false;
		const TConstArrayView<FMorphTargetDelta> Readback = Morph->GetMorphTargetDeltas(LodIndex);
		Out->SetStringField(TEXT("snapshot_id"), SnapshotId);
		Out->SetStringField(TEXT("snapshot_path"), SnapshotPath);
		Out->SetObjectField(TEXT("element"), DeltaToJson(Readback[DeltaIndex], DeltaIndex));
		Out->SetBoolField(TEXT("saved"), true);
		Summary = TEXT("Moved, saved, and read back a UE 5.8 blendshape delta element.");
		return true;
	}
	if (Name == TEXT("skeletal_blendshape_edit_rollback"))
	{
		FString SnapshotId;
		if (!Args->TryGetStringField(TEXT("snapshot_id"), SnapshotId) || SnapshotId.IsEmpty())
		{
			Error = TEXT("snapshot_id is required.");
			return false;
		}
		const FString SnapshotPath = FPaths::Combine(BlendshapeSnapshotDirectory(), SnapshotId + TEXT(".json"));
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *SnapshotPath))
		{
			Error = FString::Printf(TEXT("Blendshape snapshot was not found: %s"), *SnapshotPath);
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			Error = TEXT("Blendshape snapshot JSON is invalid.");
			return false;
		}
		FString SnapshotAsset, SnapshotMorph;
		int32 SnapshotLod = INDEX_NONE;
		Root->TryGetStringField(TEXT("asset_path"), SnapshotAsset);
		Root->TryGetStringField(TEXT("morph_target"), SnapshotMorph);
		Root->TryGetNumberField(TEXT("lod_index"), SnapshotLod);
		if (SnapshotAsset != Mesh->GetPathName() || SnapshotMorph != Morph->GetName() || SnapshotLod != LodIndex)
		{
			Error = TEXT("Snapshot target does not match asset_path, morph_target, and lod_index.");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Root->TryGetArrayField(TEXT("deltas"), Rows) || !Rows)
		{
			Error = TEXT("Snapshot does not contain deltas.");
			return false;
		}
		TArray<FMorphTargetDelta> Restored;
		Restored.Reserve(Rows->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Rows)
		{
			const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
			FMorphTargetDelta Delta;
			int32 SourceIndex = INDEX_NONE;
			if (!Row.IsValid() || !Row->TryGetNumberField(TEXT("source_index"), SourceIndex) || SourceIndex < 0 ||
				!ReadVector3f(Row, TEXT("position_delta"), Delta.PositionDelta) || !ReadVector3f(Row, TEXT("tangent_z_delta"), Delta.TangentZDelta))
			{
				Error = TEXT("Snapshot contains an invalid morph delta row.");
				return false;
			}
			Delta.SourceIdx = static_cast<uint32>(SourceIndex);
			Restored.Add(Delta);
		}
		const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
		Morph->Modify();
		Mesh->Modify();
		Morph->PopulateDeltas(Restored, LodIndex, ImportedModel->LODModels[LodIndex].Sections, true, false);
		Mesh->InitMorphTargetsAndRebuildRenderData();
		Mesh->MarkPackageDirty();
		if (!Context.Services.SaveAsset(Mesh->GetPathName(), false, Error)) return false;
		Out->SetStringField(TEXT("snapshot_id"), SnapshotId);
		Out->SetNumberField(TEXT("restored_delta_count"), Restored.Num());
		Out->SetBoolField(TEXT("saved"), true);
		Summary = TEXT("Rolled back, saved, and read back a UE 5.8 blendshape snapshot.");
		return true;
	}
	Error = FString::Printf(TEXT("Unknown blendshape tool: %s"), *Name);
	return false;
}

static void ReadNameArray(const TSharedRef<FJsonObject>& Args, const TCHAR* Field, TArray<FName>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Args->TryGetArrayField(Field, Values) || !Values) return;
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		if (Value.IsValid() && Value->Type == EJson::String && !Value->AsString().IsEmpty()) Out.AddUnique(FName(*Value->AsString()));
	}
}

static void WriteNameArray(TSharedRef<FJsonObject>& Out, const TCHAR* Field, const TArray<FName>& Names)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FName Name : Names) Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
	Out->SetArrayField(Field, Values);
}

static void WriteBoneReferenceArray(TSharedRef<FJsonObject>& Out, const TCHAR* Field, const TArray<FBoneReference>& Bones)
{
	TArray<FName> Names;
	Names.Reserve(Bones.Num());
	for (const FBoneReference& Bone : Bones) Names.Add(Bone.BoneName);
	WriteNameArray(Out, Field, Names);
}

static bool ExecuteUnusedBoneReduction(
	const FString& Name,
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Args,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FString AssetPath;
	USkeletalMesh* Mesh = LoadSkeletalMesh(Context, Args, AssetPath, Error);
	if (!Mesh) return false;
	IMeshBoneReductionModule& Module = FModuleManager::LoadModuleChecked<IMeshBoneReductionModule>(TEXT("MeshBoneReduction"));
	IMeshBoneReduction* Reduction = Module.GetMeshBoneReductionInterface();
	if (!Reduction)
	{
		Error = TEXT("MeshBoneReduction interface is unavailable.");
		return false;
	}
	TArray<FName> ForceKeep;
	ReadNameArray(Args, TEXT("force_keep_bones"), ForceKeep);
	TArray<FName> Candidates;
	Reduction->BuildBonesToBeRemovedUsedBySkinWeights(Mesh, ForceKeep, Candidates);
	Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
	Out->SetNumberField(TEXT("reference_bone_count"), Mesh->GetRefSkeleton().GetNum());
	WriteNameArray(Out, TEXT("force_keep_bones"), ForceKeep);
	WriteNameArray(Out, TEXT("unused_bone_candidates"), Candidates);
	Out->SetNumberField(TEXT("candidate_count"), Candidates.Num());

	if (Name == TEXT("skeletal_unused_bone_reduction_plan"))
	{
		Out->SetBoolField(TEXT("requires_explicit_confirmation"), true);
		Out->SetStringField(TEXT("scope"), TEXT("Skeletal Mesh LOD bone reduction; the source Skeleton asset is not destructively edited."));
		Summary = FString::Printf(TEXT("Planned UE 5.8 LOD bone reduction with %d unused candidates."), Candidates.Num());
		return true;
	}

	int32 LodIndex = 1;
	Args->TryGetNumberField(TEXT("lod_index"), LodIndex);
	if (LodIndex < 1 || !Mesh->GetLODInfo(LodIndex))
	{
		Error = TEXT("lod_index must identify an existing non-base Skeletal Mesh LOD (LOD 1 or later).");
		return false;
	}
	if (Name == TEXT("skeletal_unused_bone_reduction_execute"))
	{
		bool bConfirmed = false;
		Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
		if (!bConfirmed)
		{
			Error = TEXT("confirmed=true is required after reviewing skeletal_unused_bone_reduction_plan.");
			return false;
		}
		TArray<FName> Requested;
		ReadNameArray(Args, TEXT("bones_to_remove"), Requested);
		if (Requested.IsEmpty()) Requested = Candidates;
		for (const FName Bone : Requested)
		{
			if (!Candidates.Contains(Bone))
			{
				Error = FString::Printf(TEXT("Bone is not in the current unused-bone plan: %s"), *Bone.ToString());
				return false;
			}
			if (ForceKeep.Contains(Bone))
			{
				Error = FString::Printf(TEXT("Bone is protected by force_keep_bones: %s"), *Bone.ToString());
				return false;
			}
		}
		if (Requested.IsEmpty())
		{
			Error = TEXT("No unused bones are available for reduction.");
			return false;
		}
		const TArray<FBoneReference> Before = Mesh->GetLODInfo(LodIndex)->BonesToRemove;
		bool bIncludeBelow = false;
		Args->TryGetBoolField(TEXT("include_lower_lods"), bIncludeBelow);
		Mesh->Modify();
		Reduction->ReduceBoneCounts(Mesh, ForceKeep, Requested, LodIndex, bIncludeBelow);
		Mesh->MarkPackageDirty();
		if (!Context.Services.SaveAsset(Mesh->GetPathName(), false, Error)) return false;
		WriteBoneReferenceArray(Out, TEXT("previous_bones_to_remove"), Before);
		WriteNameArray(Out, TEXT("requested_bones_to_remove"), Requested);
		WriteBoneReferenceArray(Out, TEXT("readback_bones_to_remove"), Mesh->GetLODInfo(LodIndex)->BonesToRemove);
		Out->SetNumberField(TEXT("lod_index"), LodIndex);
		Out->SetBoolField(TEXT("include_lower_lods"), bIncludeBelow);
		Out->SetBoolField(TEXT("saved"), true);
		Summary = TEXT("Executed, saved, and read back UE 5.8 Skeletal Mesh LOD bone reduction.");
		return true;
	}
	if (Name == TEXT("skeletal_unused_bone_reduction_validate"))
	{
		TMap<FBoneIndexType, FBoneIndexType> ReplacementMap;
		const bool bHasReduction = Reduction->GetBoneReductionData(Mesh, LodIndex, ReplacementMap, nullptr);
		Out->SetNumberField(TEXT("lod_index"), LodIndex);
		Out->SetBoolField(TEXT("has_reduction_data"), bHasReduction);
		Out->SetNumberField(TEXT("replacement_count"), ReplacementMap.Num());
		WriteBoneReferenceArray(Out, TEXT("configured_bones_to_remove"), Mesh->GetLODInfo(LodIndex)->BonesToRemove);
		Out->SetBoolField(TEXT("valid"), Mesh->GetLODInfo(LodIndex)->BonesToRemove.IsEmpty() || bHasReduction);
		Summary = TEXT("Validated UE 5.8 Skeletal Mesh LOD bone reduction data without opening a modal dialog.");
		return true;
	}
	Error = FString::Printf(TEXT("Unknown unused-bone reduction tool: %s"), *Name);
	return false;
}

static void AddModuleReadback(UControlRigBlueprint* Rig, TSharedRef<FJsonObject>& Out)
{
	TArray<TSharedPtr<FJsonValue>> Modules;
	if (UModularRigController* Controller = Rig ? Rig->GetModularRigController() : nullptr)
	{
		for (const FName ModuleName : Controller->GetAllModules())
		{
			const FRigModuleReference Reference = Controller->GetModuleReference(ModuleName);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Reference.Name.ToString());
			Row->SetStringField(TEXT("parent"), Reference.ParentModuleName.ToString());
			Row->SetStringField(TEXT("source"), Reference.ControlRigAssetReference.ToSoftObjectPath().ToString());
			Row->SetNumberField(TEXT("binding_count"), Reference.Bindings.Num());
			Modules.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Out->SetArrayField(TEXT("modules"), Modules);
	Out->SetNumberField(TEXT("module_count"), Modules.Num());
}

static bool CompileSaveReadback(
	const FSololmcpToolExecutionContext& Context,
	UControlRigBlueprint* Rig,
	TSharedRef<FJsonObject>& Out,
	FString& Error)
{
	if (!Rig)
	{
		Error = TEXT("Modular Control Rig is unavailable.");
		return false;
	}
	Rig->RecompileModularRig();
	FCompilerResultsLog CompileLog;
	FKismetEditorUtilities::CompileBlueprint(Rig, EBlueprintCompileOptions::None, &CompileLog);
	Out->SetNumberField(TEXT("compile_errors"), CompileLog.NumErrors);
	Out->SetNumberField(TEXT("compile_warnings"), CompileLog.NumWarnings);
	Out->SetBoolField(TEXT("compile_succeeded"), CompileLog.NumErrors == 0);
	if (CompileLog.NumErrors > 0)
	{
		Error = FString::Printf(TEXT("Modular Control Rig compile failed with %d error(s)."), CompileLog.NumErrors);
		return false;
	}
	Rig->MarkPackageDirty();
	FString SaveError;
	const bool bSaved = Context.Services.SaveAsset(Rig->GetPathName(), false, SaveError);
	Out->SetBoolField(TEXT("saved"), bSaved);
	if (!bSaved)
	{
		Error = SaveError.IsEmpty() ? TEXT("Failed to save modular Control Rig.") : SaveError;
		return false;
	}
	AddModuleReadback(Rig, Out);
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
	Out->SetStringField(TEXT("tool"), Name);
	if (Name == TEXT("modular_control_rig_58_module_catalog"))
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FRigModuleDescription& Module : UControlRigBlueprintEditorLibrary::GetAvailableRigModules())
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("asset_path"), Module.Path.ToString());
			Row->SetStringField(TEXT("name"), Module.Settings.Identifier.Name);
			Row->SetStringField(TEXT("type"), Module.Settings.Identifier.Type);
			Row->SetStringField(TEXT("category"), Module.Settings.Category);
			Row->SetStringField(TEXT("keywords"), Module.Settings.Keywords);
			Row->SetStringField(TEXT("description"), Module.Settings.Description);
			Row->SetNumberField(TEXT("connector_count"), Module.Settings.ExposedConnectors.Num());
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Out->SetArrayField(TEXT("modules"), Rows);
		Out->SetNumberField(TEXT("module_count"), Rows.Num());
		Summary = FString::Printf(TEXT("Read %d UE 5.8 modular Control Rig module descriptions."), Rows.Num());
		return true;
	}
	if (Name == TEXT("modular_control_rig_58_module_asset_create"))
	{
		FString NewAssetPath;
		if (!Args->TryGetStringField(TEXT("asset_path"), NewAssetPath) || NewAssetPath.IsEmpty())
		{
			Error = TEXT("asset_path is required.");
			return false;
		}
		UControlRigBlueprint* ModuleRig = UControlRigBlueprintFactory::CreateNewControlRigAsset(NewAssetPath, false);
		if (!ModuleRig)
		{
			Error = FString::Printf(TEXT("Failed to create Control Rig module asset: %s"), *NewAssetPath);
			return false;
		}
		FString ConvertError;
		if (!ModuleRig->TurnIntoControlRigModule(false, &ConvertError))
		{
			Error = ConvertError.IsEmpty() ? TEXT("Failed to convert Control Rig to a module.") : ConvertError;
			return false;
		}
		FString ModuleType;
		if (Args->TryGetStringField(TEXT("module_type"), ModuleType) && !ModuleType.IsEmpty())
		{
			ModuleRig->GetRigModuleSettings().Identifier.Type = ModuleType;
		}
		FString Category;
		if (Args->TryGetStringField(TEXT("category"), Category)) ModuleRig->GetRigModuleSettings().Category = Category;
		FCompilerResultsLog CompileLog;
		FKismetEditorUtilities::CompileBlueprint(ModuleRig, EBlueprintCompileOptions::None, &CompileLog);
		Out->SetNumberField(TEXT("compile_errors"), CompileLog.NumErrors);
		Out->SetNumberField(TEXT("compile_warnings"), CompileLog.NumWarnings);
		if (CompileLog.NumErrors > 0)
		{
			Error = FString::Printf(TEXT("Control Rig module compile failed with %d error(s)."), CompileLog.NumErrors);
			return false;
		}
		ModuleRig->MarkPackageDirty();
		if (!Context.Services.SaveAsset(ModuleRig->GetPathName(), false, Error)) return false;
		FAssetRegistryModule::AssetCreated(ModuleRig);
		Out->SetStringField(TEXT("asset_path"), ModuleRig->GetPathName());
		Out->SetStringField(TEXT("identifier"), ModuleRig->GetRigModuleSettings().Identifier.Name);
		Out->SetStringField(TEXT("module_type"), ModuleRig->GetRigModuleSettings().Identifier.Type);
		Out->SetBoolField(TEXT("is_control_rig_module"), ModuleRig->IsControlRigModule());
		Out->SetBoolField(TEXT("saved"), true);
		Summary = TEXT("Created, compiled, saved, and read back a UE 5.8 Control Rig module asset.");
		return true;
	}

	FString AssetPath;
	UControlRigBlueprint* Rig = LoadModularRig(Context, Args, AssetPath, Error);
	if (!Rig) return false;
	UModularRigController* Controller = Rig->GetModularRigController();
	if (!Controller)
	{
		Error = TEXT("Modular Rig controller is unavailable.");
		return false;
	}

	if (Name == TEXT("modular_control_rig_58_compile_validate"))
	{
		const bool bOk = CompileSaveReadback(Context, Rig, Out, Error);
		if (bOk) Summary = TEXT("Compiled, saved, and read back the UE 5.8 modular Control Rig.");
		return bOk;
	}

	FString ModuleNameString;
	if (!Args->TryGetStringField(TEXT("module_name"), ModuleNameString) || ModuleNameString.IsEmpty())
	{
		Error = TEXT("module_name is required.");
		return false;
	}
	const FName ModuleName(*ModuleNameString);
	if (Name == TEXT("modular_control_rig_58_module_add"))
	{
		FString ModuleAssetPath;
		if (!Args->TryGetStringField(TEXT("module_asset_path"), ModuleAssetPath) || ModuleAssetPath.IsEmpty())
		{
			Error = TEXT("module_asset_path is required.");
			return false;
		}
		UObject* ModuleAsset = Context.Services.LoadAsset(ModuleAssetPath, Error);
		if (!ModuleAsset) return false;
		const FControlRigAssetStrongReference Source(ModuleAsset);
		if (!Source.IsValid() || !Source.IsRigModule())
		{
			Error = FString::Printf(TEXT("module_asset_path is not a valid Control Rig module: %s"), *ModuleAssetPath);
			return false;
		}
		FString ParentString;
		Args->TryGetStringField(TEXT("parent_module_name"), ParentString);
		const FName AddedName = Controller->AddModuleFromAssetReference(ModuleName, Source, FName(*ParentString), true);
		if (AddedName.IsNone())
		{
			Error = TEXT("UE 5.8 modular rig controller rejected the module add operation.");
			return false;
		}
		bool bAutoConnect = false;
		Args->TryGetBoolField(TEXT("auto_connect"), bAutoConnect);
		if (bAutoConnect) Controller->AutoConnectModules({AddedName}, false, true);
		Out->SetStringField(TEXT("added_module_name"), AddedName.ToString());
		const bool bOk = CompileSaveReadback(Context, Rig, Out, Error);
		if (bOk) Summary = TEXT("Added, compiled, saved, and read back a UE 5.8 modular Control Rig module.");
		return bOk;
	}

	if (Name == TEXT("modular_control_rig_58_module_update"))
	{
		if (!Controller->FindModule(ModuleName))
		{
			Error = FString::Printf(TEXT("Module was not found: %s"), *ModuleNameString);
			return false;
		}
		bool bChanged = false;
		FString NewNameString;
		if (Args->TryGetStringField(TEXT("new_name"), NewNameString) && !NewNameString.IsEmpty() && NewNameString != ModuleNameString)
		{
			const FName Renamed = Controller->RenameModule(ModuleName, FName(*NewNameString), true);
			if (Renamed.IsNone())
			{
				Error = TEXT("UE 5.8 modular rig controller rejected the module rename.");
				return false;
			}
			ModuleNameString = Renamed.ToString();
			bChanged = true;
		}
		const FName CurrentName(*ModuleNameString);
		FString ParentString;
		if (Args->TryGetStringField(TEXT("parent_module_name"), ParentString))
		{
			if (!Controller->ReparentModule(CurrentName, FName(*ParentString), true))
			{
				Error = TEXT("UE 5.8 modular rig controller rejected the module reparent operation.");
				return false;
			}
			bChanged = true;
		}
		FString NewSourcePath;
		if (Args->TryGetStringField(TEXT("module_asset_path"), NewSourcePath) && !NewSourcePath.IsEmpty())
		{
			UObject* NewSourceAsset = Context.Services.LoadAsset(NewSourcePath, Error);
			if (!NewSourceAsset) return false;
			const FControlRigAssetStrongReference NewSource(NewSourceAsset);
			if (!NewSource.IsValid() || !NewSource.IsRigModule() || !Controller->SwapModuleSource(CurrentName, NewSource, true))
			{
				Error = TEXT("UE 5.8 modular rig controller rejected the module source update.");
				return false;
			}
			bChanged = true;
		}
		bool bAutoConnect = false;
		if (Args->TryGetBoolField(TEXT("auto_connect"), bAutoConnect) && bAutoConnect)
		{
			Controller->AutoConnectModules({CurrentName}, false, true);
			bChanged = true;
		}
		if (!bChanged)
		{
			Error = TEXT("At least one update field is required: new_name, parent_module_name, module_asset_path, or auto_connect=true.");
			return false;
		}
		Out->SetStringField(TEXT("updated_module_name"), CurrentName.ToString());
		const bool bOk = CompileSaveReadback(Context, Rig, Out, Error);
		if (bOk) Summary = TEXT("Updated, compiled, saved, and read back a UE 5.8 modular Control Rig module.");
		return bOk;
	}

	Error = FString::Printf(TEXT("Unknown UE 5.8 character/animation tool: %s"), *Name);
	return false;
}
#endif
}

void RegisterUE58CharacterAnimationTools(FSololmcpToolRegistry& Registry)
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
	using namespace UE58CharacterAnimation;
	const auto Register = [&Registry](const TCHAR* Name, const TCHAR* Description, const TSharedRef<FJsonObject>& Schema, int32 CacheTtl = 0)
	{
		Registry.Register({Name, Description, Schema,
			[Name](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
			{
				return Execute(Name, Context, Args, Out, Summary, Error);
			}, nullptr, CacheTtl});
	};
	const auto RegisterBlendshape = [&Registry](const TCHAR* Name, const TCHAR* Description, const TSharedRef<FJsonObject>& Schema, int32 CacheTtl = 0)
	{
		Registry.Register({Name, Description, Schema,
			[Name](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
			{
				return ExecuteBlendshape(Name, Context, Args, Out, Summary, Error);
			}, nullptr, CacheTtl});
	};
	const auto RegisterBoneReduction = [&Registry](const TCHAR* Name, const TCHAR* Description, const TSharedRef<FJsonObject>& Schema, int32 CacheTtl = 0)
	{
		Registry.Register({Name, Description, Schema,
			[Name](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
			{
				return ExecuteUnusedBoneReduction(Name, Context, Args, Out, Summary, Error);
			}, nullptr, CacheTtl});
	};
	const TSharedRef<FJsonObject> BlendshapeElementSchema = FSololmcpSchemaBuilder::Object({
		{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
		{TEXT("morph_target"), FSololmcpSchemaBuilder::String()},
		{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer()},
		{TEXT("delta_index"), FSololmcpSchemaBuilder::Integer()}},
		{TEXT("asset_path"), TEXT("morph_target"), TEXT("delta_index")});
	RegisterBlendshape(TEXT("skeletal_blendshape_mesh_element_inspect"), TEXT("Inspect one UE 5.8 Skeletal Mesh morph-target delta by LOD and delta index."), BlendshapeElementSchema, 5);
	RegisterBlendshape(TEXT("skeletal_blendshape_mesh_element_move"), TEXT("Move one UE 5.8 morph-target delta with automatic snapshot, save, and readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("morph_target"), FSololmcpSchemaBuilder::String()},
			{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("delta_index"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("offset"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number())}},
			{TEXT("asset_path"), TEXT("morph_target"), TEXT("delta_index"), TEXT("offset")}));
	RegisterBlendshape(TEXT("skeletal_blendshape_edit_snapshot"), TEXT("Persist a project-local snapshot of one UE 5.8 morph target LOD before editing."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("morph_target"), FSololmcpSchemaBuilder::String()},
			{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("morph_target")}));
	RegisterBlendshape(TEXT("skeletal_blendshape_edit_rollback"), TEXT("Restore a UE 5.8 morph target LOD from a project-local SOMOLMCP snapshot."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("morph_target"), FSololmcpSchemaBuilder::String()},
			{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("snapshot_id"), FSololmcpSchemaBuilder::String()}},
			{TEXT("asset_path"), TEXT("morph_target"), TEXT("snapshot_id")}));
	RegisterBoneReduction(TEXT("skeletal_unused_bone_reduction_plan"), TEXT("Find bones unused by skin weights and build a non-modal, fail-closed UE 5.8 LOD reduction plan."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("force_keep_bones"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}}, {TEXT("asset_path")}), 5);
	RegisterBoneReduction(TEXT("skeletal_unused_bone_reduction_execute"), TEXT("Apply an explicitly confirmed UE 5.8 unused-bone plan to non-base Skeletal Mesh LODs, then save and read back."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("lod_index"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("force_keep_bones"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
			{TEXT("bones_to_remove"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
			{TEXT("include_lower_lods"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("confirmed"), FSololmcpSchemaBuilder::Boolean()}},
			{TEXT("asset_path"), TEXT("lod_index"), TEXT("confirmed")}));
	RegisterBoneReduction(TEXT("skeletal_unused_bone_reduction_validate"), TEXT("Validate UE 5.8 Skeletal Mesh LOD bone replacement data and configured removals."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("lod_index"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("force_keep_bones"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}},
			{TEXT("asset_path"), TEXT("lod_index")}), 5);
	Register(TEXT("modular_control_rig_58_module_catalog"), TEXT("List UE 5.8 Control Rig modules available to modular rig authoring."),
		FSololmcpSchemaBuilder::Object({}), 5);
	Register(TEXT("modular_control_rig_58_module_asset_create"), TEXT("Create a real UE 5.8 Control Rig module asset for modular rig assembly."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("module_type"), FSololmcpSchemaBuilder::String()},
			{TEXT("category"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));
	Register(TEXT("modular_control_rig_58_module_add"), TEXT("Add a real module asset to a UE 5.8 modular Control Rig, compile, save, and read back."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("module_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("module_asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("parent_module_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("auto_connect"), FSololmcpSchemaBuilder::Boolean()}},
			{TEXT("asset_path"), TEXT("module_name"), TEXT("module_asset_path")}));
	Register(TEXT("modular_control_rig_58_module_update"), TEXT("Rename, reparent, reconnect, or replace the source of a UE 5.8 modular Control Rig module."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("module_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("new_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("parent_module_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("module_asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("auto_connect"), FSololmcpSchemaBuilder::Boolean()}},
			{TEXT("asset_path"), TEXT("module_name")}));
	Register(TEXT("modular_control_rig_58_compile_validate"), TEXT("Compile, save, and read back a UE 5.8 modular Control Rig with fail-closed diagnostics."),
		FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));
#endif
}
}
#else
namespace UE::SOMOLMCP
{
void RegisterUE58CharacterAnimationTools(FSololmcpToolRegistry&)
{
}
}
#endif
