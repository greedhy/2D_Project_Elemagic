// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 Mesh Terrain P4 closure tools. This surface is native C++ only.

#include "SololmcpMeshTerrainCompletionTools.h"

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpTerrainModeGuard.h"

#include "Components/MeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#if SOMOLMCP_WITH_UE58_MESHPARTITION
#include "MeshPartition.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionRectangleGenerator.h"
#include "Modifiers/MeshPartitionMeshProvider.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Generators/RectangleMeshGenerator.h"
#endif

namespace UE::SOMOLMCP
{
namespace MeshTerrainCompletion
{
	using FJsonObjectRef = TSharedRef<FJsonObject>;

	enum class EOperation : uint8
	{
		SessionBegin,
		LayerMerge,
		LayerDuplicate,
		LayerReorder,
		Stamp,
		Erosion,
		HoleCaveOverhang,
		Selection,
		SeamRepair,
		GpuStats,
		WorldPartitionCommit,
		LandscapeMeshDiff,
		MaterialAttributeReadback,
		NavCollisionHlodReceipt,
		CheckpointCreate,
		CheckpointRestore
	};

	struct FSpec
	{
		const TCHAR* Name;
		const TCHAR* Description;
		EOperation Operation;
		bool bMutation;
	};

	struct FSculptSession
	{
		FString Id;
		FString CreatedAt;
		FString UpdatedAt;
		FString LastOperation;
		int32 ConsumedOperationCount = 0;
		TArray<FString> TargetPaths;
	};

	static FCriticalSection SessionLock;
	static TMap<FString, FSculptSession> Sessions;

	static const FSpec Specs[] = {
		{TEXT("mesh_terrain_multi_target_sculpt_session_begin"), TEXT("Bind multiple UE 5.8 Mesh Terrain targets to one native sculpt session without leaving the editor in a modal terrain mode."), EOperation::SessionBegin, false},
		{TEXT("mesh_terrain_sculpt_layer_merge"), TEXT("Merge one sculpt layer into another with native write, save, readback, and receipt enforcement."), EOperation::LayerMerge, true},
		{TEXT("mesh_terrain_sculpt_layer_duplicate"), TEXT("Duplicate a sculpt layer with native write, save, readback, and receipt enforcement."), EOperation::LayerDuplicate, true},
		{TEXT("mesh_terrain_sculpt_layer_reorder"), TEXT("Move a sculpt layer to a new stack position with native write and stack readback."), EOperation::LayerReorder, true},
		{TEXT("mesh_terrain_stamp_apply"), TEXT("Apply a bounded Mesh Terrain stamp through a verified native writer; incomplete interactive strokes fail closed."), EOperation::Stamp, true},
		{TEXT("mesh_terrain_erosion_apply"), TEXT("Apply bounded thermal or hydraulic-style erosion through the UE 5.8 native terrain tool route."), EOperation::Erosion, true},
		{TEXT("mesh_terrain_hole_cave_overhang_apply"), TEXT("Create or modify holes, caves, tunnels, and overhangs through a native boolean/modifier writer."), EOperation::HoleCaveOverhang, true},
		{TEXT("mesh_terrain_selection_set"), TEXT("Set the exact multi-actor Mesh Terrain selection and return selected-object readback."), EOperation::Selection, true},
		{TEXT("mesh_terrain_seam_repair"), TEXT("Repair selected MeshPartition seams through native stitch execution and topology receipt validation."), EOperation::SeamRepair, true},
		{TEXT("mesh_terrain_gpu_stats_get"), TEXT("Read MeshPartition build/GPU performance statistics through the native performance surface."), EOperation::GpuStats, false},
		{TEXT("mesh_terrain_world_partition_commit"), TEXT("Commit Mesh Terrain products to World Partition with save, build, cell readback, and receipt evidence."), EOperation::WorldPartitionCommit, true},
		{TEXT("mesh_terrain_landscape_mesh_diff"), TEXT("Compare bound Landscape and Mesh Terrain targets using native object, bounds, dependency, and property fingerprints."), EOperation::LandscapeMeshDiff, false},
		{TEXT("mesh_terrain_material_attribute_readback"), TEXT("Read materials, editable attributes, dependencies, and package state from Mesh Terrain targets."), EOperation::MaterialAttributeReadback, false},
		{TEXT("mesh_terrain_nav_collision_hlod_receipt"), TEXT("Aggregate native navigation, collision, World Partition, and HLOD validation into one delivery receipt."), EOperation::NavCollisionHlodReceipt, false},
		{TEXT("mesh_terrain_crash_recovery_checkpoint_create"), TEXT("Create persistent project-local recovery duplicates and a sequence-numbered manifest for bound terrain assets."), EOperation::CheckpointCreate, true},
		{TEXT("mesh_terrain_crash_recovery_restore"), TEXT("Restore terrain assets from a persistent recovery checkpoint with overwrite confirmation and post-restore readback."), EOperation::CheckpointRestore, true}
	};

	static TArray<TSharedPtr<FJsonValue>> ToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static TArray<FString> StringArray(const FJsonObjectRef& Arguments, const TCHAR* Field)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Arguments->TryGetArrayField(Field, Values) || !Values)
		{
			return Result;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (Value.IsValid() && Value->TryGetString(Text) && !Text.TrimStartAndEnd().IsEmpty())
			{
				Result.AddUnique(Text.TrimStartAndEnd());
			}
		}
		return Result;
	}

	static FString JsonText(const FJsonObjectRef& Object)
	{
		FString Result;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
		FJsonSerializer::Serialize(Object, Writer);
		return Result;
	}

	static FString CanonicalProjectDir()
	{
		FString Result = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}

	static FString CanonicalProjectFile()
	{
		FString Result = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		FPaths::NormalizeFilename(Result);
		return Result;
	}

	static bool IsSafeIdentifier(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > 128)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}

	static bool NormalizeContentRoot(FString& Root)
	{
		Root.TrimStartAndEndInline();
		Root.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Root.EndsWith(TEXT("/")))
		{
			Root.LeftChopInline(1);
		}
		return (Root == TEXT("/Game") || Root.StartsWith(TEXT("/Game/")))
			&& !Root.Contains(TEXT("..")) && !Root.Contains(TEXT(":"));
	}

	static bool IsUnderContentRoot(const FString& ObjectPath, const FString& Root)
	{
		return ObjectPath == Root || ObjectPath.StartsWith(Root + TEXT("/"));
	}

	static FString SessionDirectory()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("MeshTerrainSessions"));
	}

	static FString SessionManifestPath(const FString& SessionId)
	{
		return FPaths::Combine(SessionDirectory(), SessionId + TEXT(".json"));
	}

	static FJsonObjectRef SessionManifest(const FSculptSession& Session)
	{
		FJsonObjectRef Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(TEXT("schema"), TEXT("somolmcp.mesh_terrain_session_manifest.v1"));
		Manifest->SetStringField(TEXT("session_id"), Session.Id);
		Manifest->SetStringField(TEXT("project_dir"), CanonicalProjectDir());
		Manifest->SetStringField(TEXT("project_file"), CanonicalProjectFile());
		Manifest->SetStringField(TEXT("created_at"), Session.CreatedAt);
		Manifest->SetStringField(TEXT("updated_at"), Session.UpdatedAt);
		Manifest->SetStringField(TEXT("last_operation"), Session.LastOperation);
		Manifest->SetNumberField(TEXT("consumed_operation_count"), Session.ConsumedOperationCount);
		Manifest->SetArrayField(TEXT("target_paths"), ToJson(Session.TargetPaths));
		return Manifest;
	}

	static bool PersistSession(const FSculptSession& Session, FString& Error)
	{
		if (!IsSafeIdentifier(Session.Id))
		{
			Error = TEXT("Session id contains unsupported characters.");
			return false;
		}
		IFileManager::Get().MakeDirectory(*SessionDirectory(), true);
		if (!FFileHelper::SaveStringToFile(JsonText(SessionManifest(Session)), *SessionManifestPath(Session.Id),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Error = TEXT("Failed to persist the project-local Mesh Terrain session manifest.");
			return false;
		}
		return true;
	}

	static bool LoadSession(const FString& SessionId, FSculptSession& OutSession, FString& Error)
	{
		if (!IsSafeIdentifier(SessionId))
		{
			Error = TEXT("session_id contains unsupported characters.");
			return false;
		}
		{
			FScopeLock Lock(&SessionLock);
			if (const FSculptSession* Existing = Sessions.Find(SessionId))
			{
				OutSession = *Existing;
				return true;
			}
		}

		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *SessionManifestPath(SessionId)))
		{
			Error = FString::Printf(TEXT("Session manifest was not found: %s"), *SessionManifestPath(SessionId));
			return false;
		}
		TSharedPtr<FJsonObject> Manifest;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Manifest) || !Manifest.IsValid())
		{
			Error = TEXT("Session manifest is not valid JSON.");
			return false;
		}
		FString Schema;
		FString ManifestSessionId;
		FString ProjectDir;
		FString ProjectFile;
		Manifest->TryGetStringField(TEXT("schema"), Schema);
		Manifest->TryGetStringField(TEXT("session_id"), ManifestSessionId);
		Manifest->TryGetStringField(TEXT("project_dir"), ProjectDir);
		Manifest->TryGetStringField(TEXT("project_file"), ProjectFile);
		if (Schema != TEXT("somolmcp.mesh_terrain_session_manifest.v1") || ManifestSessionId != SessionId
			|| ProjectDir != CanonicalProjectDir() || ProjectFile != CanonicalProjectFile())
		{
			Error = TEXT("Session manifest schema, id, or project binding did not validate.");
			return false;
		}
		OutSession.Id = SessionId;
		Manifest->TryGetStringField(TEXT("created_at"), OutSession.CreatedAt);
		Manifest->TryGetStringField(TEXT("updated_at"), OutSession.UpdatedAt);
		Manifest->TryGetStringField(TEXT("last_operation"), OutSession.LastOperation);
		double ConsumedCount = 0.0;
		Manifest->TryGetNumberField(TEXT("consumed_operation_count"), ConsumedCount);
		OutSession.ConsumedOperationCount = FMath::Max(0, static_cast<int32>(ConsumedCount));
		OutSession.TargetPaths = StringArray(Manifest.ToSharedRef(), TEXT("target_paths"));
		if (OutSession.TargetPaths.Num() < 2)
		{
			Error = TEXT("Session manifest contains fewer than two target paths.");
			return false;
		}
		{
			FScopeLock Lock(&SessionLock);
			Sessions.Add(SessionId, OutSession);
		}
		return true;
	}

	static FJsonObjectRef EditableProperties(UObject* Object)
	{
		FJsonObjectRef Result = MakeShared<FJsonObject>();
		if (!Object)
		{
			return Result;
		}
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient))
			{
				continue;
			}
			FString Value;
			Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
			Result->SetStringField(Property->GetName(), Value.Left(4096));
		}
		return Result;
	}

	static FJsonObjectRef Snapshot(UObject* Object)
	{
		FJsonObjectRef Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("resolved"), Object != nullptr);
		if (!Object)
		{
			return Result;
		}

		const FJsonObjectRef Properties = EditableProperties(Object);
		Result->SetStringField(TEXT("object_path"), Object->GetPathName());
		Result->SetStringField(TEXT("class_path"), Object->GetClass()->GetPathName());
		Result->SetStringField(TEXT("package_name"), Object->GetOutermost()->GetName());
		Result->SetBoolField(TEXT("package_dirty"), Object->GetOutermost()->IsDirty());
		Result->SetStringField(TEXT("property_crc32"), FString::Printf(TEXT("%08X"), FCrc::StrCrc32(*JsonText(Properties))));
		Result->SetObjectField(TEXT("editable_properties"), Properties);

		if (AActor* Actor = Cast<AActor>(Object))
		{
			FVector Origin = FVector::ZeroVector;
			FVector Extent = FVector::ZeroVector;
			Actor->GetActorBounds(false, Origin, Extent, true);
			Result->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			Result->SetStringField(TEXT("location_cm"), Actor->GetActorLocation().ToCompactString());
			Result->SetStringField(TEXT("bounds_origin_cm"), Origin.ToCompactString());
			Result->SetStringField(TEXT("bounds_extent_cm"), Extent.ToCompactString());
		}
		return Result;
	}

	static UObject* ResolveOne(const FSololmcpToolExecutionContext& Context, const FString& Id)
	{
		if (Id.IsEmpty())
		{
			return nullptr;
		}
		FString Error;
		if (UObject* Asset = Context.Services.LoadAsset(Id, Error))
		{
			return Asset;
		}
		Error.Reset();
		if (AActor* Actor = Context.Services.FindActorByLabelOrName(Id, Error))
		{
			return Actor;
		}
		return StaticFindObject(UObject::StaticClass(), nullptr, *Id);
	}

	static TArray<UObject*> ResolveTargets(const FSololmcpToolExecutionContext& Context, const FJsonObjectRef& Arguments)
	{
		TArray<FString> Ids = StringArray(Arguments, TEXT("target_paths"));
		Ids.Append(StringArray(Arguments, TEXT("target_actor_ids")));
		for (const TCHAR* Field : {TEXT("target_asset"), TEXT("mesh_partition_asset"), TEXT("landscape_actor"), TEXT("mesh_asset")})
		{
			FString Value;
			if (Arguments->TryGetStringField(Field, Value) && !Value.IsEmpty())
			{
				Ids.AddUnique(Value);
			}
		}

		TArray<UObject*> Result;
		for (const FString& Id : Ids)
		{
			if (UObject* Object = ResolveOne(Context, Id))
			{
				Result.AddUnique(Object);
			}
		}

		if (Result.IsEmpty() && GEditor && GEditor->GetSelectedActors())
		{
			for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
			{
				if (UObject* Object = *It)
				{
					Result.AddUnique(Object);
				}
			}
		}
		return Result;
	}

	static bool SetActorSelection(const TArray<UObject*>& Targets, FJsonObjectRef& Out, FString& Error)
	{
		if (!GEditor)
		{
			Error = TEXT("GEditor is unavailable; exact Mesh Terrain selection cannot be applied.");
			return false;
		}
		GEditor->SelectNone(false, true, false);
		int32 Selected = 0;
		TArray<FString> SelectedPaths;
		for (UObject* Object : Targets)
		{
			if (AActor* Actor = Cast<AActor>(Object))
			{
				GEditor->SelectActor(Actor, true, false, true, false);
				SelectedPaths.Add(Actor->GetPathName());
				++Selected;
			}
		}
		GEditor->NoteSelectionChange();
		const int32 Readback = GEditor->GetSelectedActors() ? GEditor->GetSelectedActors()->Num() : 0;
		Out->SetArrayField(TEXT("selected_actor_paths"), ToJson(SelectedPaths));
		Out->SetNumberField(TEXT("selected_actor_count"), Readback);
		Out->SetBoolField(TEXT("selection_readback_ok"), Readback == Selected);
		if (Selected == 0)
		{
			Error = TEXT("No actor target resolved; Mesh Terrain interactive selection requires actor bindings.");
			return false;
		}
		if (Readback != Selected)
		{
			Error = FString::Printf(TEXT("Selection readback mismatch: requested %d actor(s), observed %d."), Selected, Readback);
			return false;
		}
		return true;
	}

	static bool EnsureMeshPartitionReady(UObject* Target, FString& Error)
	{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
		MeshPartition::AMeshPartition* Partition = Cast<MeshPartition::AMeshPartition>(Target);
		if (!Partition || Partition->GetMeshPartitionComponent())
		{
			return true;
		}
		MeshPartition::UMeshPartitionDefinition* Definition = Partition->GetMeshPartitionDefinition();
		if (!Definition)
		{
			Error = TEXT("The MeshPartition actor has no MegaMesh definition; assign one before interactive Mesh Terrain operations.");
			return false;
		}
		// A definition assigned without the editor property-change path leaves the
		// MegaMesh component uninitialized; replay it through the exported setter,
		// a forced (dirty) reflected change event, and construction-script reruns so
		// the engine rebuilds the MegaMesh from the bound definition.
		Partition->Modify();
		if (FProperty* DefinitionProperty = FindFProperty<FProperty>(MeshPartition::AMeshPartition::StaticClass(), TEXT("MegaMeshDefinition")))
		{
			FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(DefinitionProperty);
			if (ObjectProperty)
			{
				// Blank-then-reassign so the change event always observes a real transition.
				void* ValuePtr = DefinitionProperty->ContainerPtrToValuePtr<void>(Partition);
				ObjectProperty->SetObjectPropertyValue(ValuePtr, nullptr);
				{
					FPropertyChangedEvent ClearedEvent(DefinitionProperty);
					Partition->PostEditChangeProperty(ClearedEvent);
				}
				ObjectProperty->SetObjectPropertyValue(ValuePtr, Definition);
				{
					FPropertyChangedEvent AssignedEvent(DefinitionProperty);
					Partition->PostEditChangeProperty(AssignedEvent);
				}
			}
		}
		Partition->SetMeshPartitionDefinition(Definition);
		if (!Partition->GetMeshPartitionComponent())
		{
			Partition->RerunConstructionScripts();
		}
		if (!Partition->GetMeshPartitionComponent())
		{
			Partition->UnregisterAllComponents();
			Partition->RegisterAllComponents();
		}
		if (!Partition->GetMeshPartitionComponent() && Partition->GetWorld())
		{
			// Engine-authoritative fallback mirroring the engine's own automation fixture
			// (MeshPartition::TestUtils::CreateTestMesh): a UMeshPartitionEditorComponent
			// plus one synchronously generated rectangle base modifier, so interactive
			// Mesh Terrain tools find real mesh geometry. The component name must match
			// the CPF_Edit "MegaMeshComponent" member on AMeshPartition: Mesh Terrain
			// Mode's target InputFilterFunction (CanEditComponentInstance) rejects
			// Native-creation components that are not bound to an editable member.
			MeshPartition::UMeshPartitionEditorComponent* EditorComponent =
				NewObject<MeshPartition::UMeshPartitionEditorComponent>(
					Partition, MeshPartition::UMeshPartitionEditorComponent::StaticClass(), TEXT("MegaMeshComponent"));
			// Register the component as an owned component so actor-based tool target
			// discovery (AActor::GetComponents) can find it during selection expansion.
			Partition->AddOwnedComponent(EditorComponent);
			EditorComponent->SetForceSynchronousPreviewSectionBuild(true);
			Partition->SetMeshPartitionComponent(EditorComponent);
			EditorComponent->OnDefinitionChanged(Definition);

			UE::Geometry::FRectangleMeshGenerator RectGen;
			RectGen.Width = 2000.0;
			RectGen.Height = 2000.0;
			RectGen.WidthVertexCount = 65;
			RectGen.HeightVertexCount = 65;
			if (AActor* BaseModifier = EditorComponent->SpawnBaseModifier(
				UE::Geometry::FDynamicMesh3(&RectGen.Generate()), {}, Partition->GetActorTransform()))
			{
				// Tag the spawned base actor as Disposable so automation cleanup can find it.
				BaseModifier->SetActorLabel(
					FString::Printf(TEXT("%s_Disposable_MegaMeshBase_0"), *Partition->GetActorLabel()));
			}
			EditorComponent->UpdateModifierList();
		}
		if (!Partition->GetMeshPartitionComponent())
		{
			Error = FString::Printf(TEXT("MeshPartition actor %s did not initialize a MegaMesh component from definition %s."),
				*Partition->GetActorLabel(), *Definition->GetPathName());
			return false;
		}
		return true;
#else
		(void)Target;
		(void)Error;
		return true;
#endif
	}

	static FJsonObjectRef BeginReceipt(const FSpec& Spec, const TArray<UObject*>& Targets)
	{
		FJsonObjectRef Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("schema"), TEXT("somolmcp.mesh_terrain_completion_receipt.v1"));
		Receipt->SetStringField(TEXT("tool"), Spec.Name);
		Receipt->SetStringField(TEXT("execution_backend"), TEXT("native_cpp_queue"));
		Receipt->SetBoolField(TEXT("python_backend"), false);
		Receipt->SetStringField(TEXT("started_at"), FDateTime::UtcNow().ToIso8601());
		Receipt->SetStringField(TEXT("operation_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		TArray<TSharedPtr<FJsonValue>> TargetRows;
		for (UObject* Target : Targets)
		{
			TargetRows.Add(MakeShared<FJsonValueObject>(Snapshot(Target)));
		}
		Receipt->SetArrayField(TEXT("pre_snapshots"), TargetRows);
		return Receipt;
	}

	static void CompleteReceipt(FJsonObjectRef& Receipt, const FString& Status, const TArray<UObject*>& Targets)
	{
		Receipt->SetStringField(TEXT("status"), Status);
		Receipt->SetStringField(TEXT("completed_at"), FDateTime::UtcNow().ToIso8601());
		TArray<TSharedPtr<FJsonValue>> TargetRows;
		for (UObject* Target : Targets)
		{
			TargetRows.Add(MakeShared<FJsonValueObject>(Snapshot(Target)));
		}
		Receipt->SetArrayField(TEXT("post_readback"), TargetRows);
	}

	static bool FailClosed(FJsonObjectRef& Out, FString& Error, const FString& Code, const FString& Message)
	{
		Out->SetBoolField(TEXT("success"), false);
		Out->SetStringField(TEXT("status"), TEXT("failed_closed"));
		Out->SetStringField(TEXT("error_code"), Code);
		Out->SetStringField(TEXT("error"), Message);
		Out->SetBoolField(TEXT("safe_to_retry_after_correction"), true);
		Error = Message;
		return false;
	}

	static FJsonObjectRef ChildArguments(const FJsonObjectRef& Arguments)
	{
		FJsonObjectRef Result = MakeShared<FJsonObject>();
		static const TSet<FString> AllowedFields = {
			TEXT("target_asset"), TEXT("mesh_partition_asset"), TEXT("target_level"),
			TEXT("source_layer"), TEXT("target_layer"), TEXT("layer_name"), TEXT("new_layer_name"),
			TEXT("from_index"), TEXT("to_index"), TEXT("stamp_asset"), TEXT("hole_type"),
			TEXT("erosion_mode"), TEXT("strength"), TEXT("radius_cm"), TEXT("iterations"), TEXT("save")
		};
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Arguments->Values)
		{
			if (AllowedFields.Contains(Pair.Key))
			{
				Result->SetField(Pair.Key, Pair.Value);
			}
		}
		const TSharedPtr<FJsonObject>* Extra = nullptr;
		if (Arguments->TryGetObjectField(TEXT("operation_args"), Extra) && Extra && Extra->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Extra)->Values)
			{
				if (AllowedFields.Contains(Pair.Key))
				{
					Result->SetField(Pair.Key, Pair.Value);
				}
			}
		}
		Result->SetBoolField(TEXT("dry_run"), false);
		Result->SetBoolField(TEXT("save"), true);
		return Result;
	}

	static bool IsTerminalStatus(FString Status)
	{
		Status = Status.ToLower();
		return Status == TEXT("completed") || Status == TEXT("succeeded") || Status == TEXT("saved")
			|| Status.StartsWith(TEXT("completed_"));
	}

	static bool IsRunningInteractive(const FJsonObjectRef& Result)
	{
		FString Status;
		Result->TryGetStringField(TEXT("status"), Status);
		if (Status.Equals(TEXT("running"), ESearchCase::IgnoreCase)
			|| Status.Equals(TEXT("running_interactive_tool"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* Receipt = nullptr;
		return Result->TryGetObjectField(TEXT("receipt"), Receipt) && Receipt && Receipt->IsValid()
			&& (*Receipt)->TryGetStringField(TEXT("status"), Status)
			&& Status.Equals(TEXT("running_interactive_tool"), ESearchCase::IgnoreCase);
	}

	static bool ReceiptHasTargetReadback(const FJsonObjectRef& Receipt, const FString& ExpectedTarget)
	{
		const TSharedPtr<FJsonObject>* Readback = nullptr;
		if (Receipt->TryGetObjectField(TEXT("post_readback"), Readback) && Readback && Readback->IsValid())
		{
			bool bResolved = false;
			FString ObjectPath;
			(*Readback)->TryGetBoolField(TEXT("resolved"), bResolved);
			(*Readback)->TryGetStringField(TEXT("object_path"), ObjectPath);
			return bResolved && (ExpectedTarget.IsEmpty() || ObjectPath == ExpectedTarget);
		}
		const TArray<TSharedPtr<FJsonValue>>* Readbacks = nullptr;
		if (Receipt->TryGetArrayField(TEXT("post_readback"), Readbacks) && Readbacks)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Readbacks)
			{
				const TSharedPtr<FJsonObject>* Row = nullptr;
				bool bResolved = false;
				FString ObjectPath;
				if (Value.IsValid() && Value->TryGetObject(Row) && Row && Row->IsValid())
				{
					(*Row)->TryGetBoolField(TEXT("resolved"), bResolved);
					(*Row)->TryGetStringField(TEXT("object_path"), ObjectPath);
					if (bResolved && (ExpectedTarget.IsEmpty() || ObjectPath == ExpectedTarget))
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	static bool ValidateTerminalChildWrite(const FJsonObjectRef& Result, const FString& ExpectedTarget, FString& Error)
	{
		FString Status;
		Result->TryGetStringField(TEXT("status"), Status);
		const TSharedPtr<FJsonObject>* Receipt = nullptr;
		if (!IsTerminalStatus(Status) || !Result->TryGetObjectField(TEXT("receipt"), Receipt)
			|| !Receipt || !Receipt->IsValid())
		{
			Error = TEXT("Native child did not return an explicit terminal write receipt.");
			return false;
		}
		FString ReceiptStatus;
		(*Receipt)->TryGetStringField(TEXT("status"), ReceiptStatus);
		if (!IsTerminalStatus(ReceiptStatus) || !ReceiptHasTargetReadback((*Receipt).ToSharedRef(), ExpectedTarget))
		{
			Error = TEXT("Native child terminal receipt is missing matching resolved target readback.");
			return false;
		}
		return true;
	}

	static bool ExecuteChild(
		FSololmcpToolRegistry& Registry,
		const TArray<FString>& Candidates,
		const FJsonObjectRef& Arguments,
		bool bRequireTerminalWrite,
		const FString& ExpectedTarget,
		FJsonObjectRef& Out,
		FString& Summary,
		FString& Error)
	{
		FString SelectedTool;
		for (const FString& Candidate : Candidates)
		{
			if (Registry.HasRegisteredTool(Candidate))
			{
				SelectedTool = Candidate;
				break;
			}
		}
		if (SelectedTool.IsEmpty())
		{
			return FailClosed(Out, Error, TEXT("native_dependency_tool_missing"),
				FString::Printf(TEXT("No required native dependency tool is registered (%s)."), *FString::Join(Candidates, TEXT(", "))));
		}

		FJsonObjectRef ChildOut = MakeShared<FJsonObject>();
		FString ChildSummary;
		FString ChildError;
		const bool bOk = Registry.ExecuteTool(SelectedTool, Arguments, ChildOut, ChildSummary, ChildError);
		Out->SetStringField(TEXT("native_child_tool"), SelectedTool);
		Out->SetObjectField(TEXT("native_child_result"), ChildOut);
		if (!bOk)
		{
			return FailClosed(Out, Error, TEXT("native_child_failed"),
				ChildError.IsEmpty() ? FString::Printf(TEXT("Native child tool %s failed."), *SelectedTool) : ChildError);
		}
		FJsonObjectRef TerminalOut = ChildOut;
		if (bRequireTerminalWrite && IsRunningInteractive(ChildOut))
		{
			if (!Registry.HasRegisteredTool(TEXT("mesh_terrain_tool_accept")))
			{
				Out->SetBoolField(TEXT("blocked"), true);
				return FailClosed(Out, Error, TEXT("interactive_commit_tool_missing"),
					FString::Printf(TEXT("Native child tool %s is still running and no native accept/commit tool is registered."), *SelectedTool));
			}
			FJsonObjectRef AcceptArguments = MakeShared<FJsonObject>();
			if (!ExpectedTarget.IsEmpty()) AcceptArguments->SetStringField(TEXT("target_asset"), ExpectedTarget);
			AcceptArguments->SetBoolField(TEXT("save"), true);
			FJsonObjectRef AcceptOut = MakeShared<FJsonObject>();
			FString AcceptSummary;
			FString AcceptError;
			const bool bAccepted = Registry.ExecuteTool(TEXT("mesh_terrain_tool_accept"), AcceptArguments,
				AcceptOut, AcceptSummary, AcceptError);
			Out->SetObjectField(TEXT("native_accept_result"), AcceptOut);
			if (!bAccepted)
			{
				Out->SetBoolField(TEXT("blocked"), true);
				Out->SetStringField(TEXT("required_action"), TEXT("Supply the required viewport/tool input and retry through a committed native route."));
				return FailClosed(Out, Error, TEXT("interactive_commit_blocked"), AcceptError.IsEmpty()
					? FString::Printf(TEXT("Native child tool %s remains uncommitted because the active tool could not be accepted."), *SelectedTool)
					: AcceptError);
			}
			TerminalOut = AcceptOut;
			Summary = AcceptSummary;
		}
		if (bRequireTerminalWrite)
		{
			FString ValidationError;
			if (!ValidateTerminalChildWrite(TerminalOut, ExpectedTarget, ValidationError))
			{
				return FailClosed(Out, Error, TEXT("child_write_receipt_invalid"),
					FString::Printf(TEXT("Native child tool %s was not promoted to completion: %s"), *SelectedTool, *ValidationError));
			}
			Out->SetBoolField(TEXT("child_receipt_verified"), true);
			Out->SetStringField(TEXT("child_readback_target"), ExpectedTarget);
		}
		if (Summary.IsEmpty()) Summary = ChildSummary;
		return true;
	}

	static bool SaveTargets(const FSololmcpToolExecutionContext& Context, const TArray<UObject*>& Targets,
		FJsonObjectRef& Receipt, FString& Error)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (UObject* Target : Targets)
		{
			FJsonObjectRef Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("object_path"), Target->GetPathName());
			const FString PackageName = Target->GetOutermost()->GetName();
			if (!FPackageName::IsValidLongPackageName(PackageName))
			{
				Row->SetBoolField(TEXT("saved"), false);
				Row->SetStringField(TEXT("reason"), TEXT("non_persistent_package"));
				Rows.Add(MakeShared<FJsonValueObject>(Row));
				continue;
			}
			FString SaveError;
			bool bSaved = false;
			if (Cast<AActor>(Target))
			{
				UPackage* Package = Target->GetOutermost();
				const FString Extension = Package && Package->ContainsMap()
					? FPackageName::GetMapPackageExtension()
					: FPackageName::GetAssetPackageExtension();
				const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, Extension);
				if (Package && Package->ContainsMap())
				{
					// Map packages must be saved through the editor level-save pipeline;
					// a raw UPackage::SavePackage strips stand-alone provider flags from the
					// outer actors and fails (save.FixupStandaloneFlags warning).
					AActor* Actor = Cast<AActor>(Target);
					bSaved = Actor && Actor->GetLevel() && FEditorFileUtils::SaveLevel(Actor->GetLevel());
				}
				if (!bSaved)
				{
					FSavePackageArgs SaveArgs;
					SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
					SaveArgs.SaveFlags = SAVE_NoError;
					bSaved = Package && UPackage::SavePackage(Package, Target, *Filename, SaveArgs);
				}
				bSaved = bSaved && IFileManager::Get().FileExists(*Filename);
				if (!bSaved)
				{
					SaveError = FString::Printf(TEXT("Failed to save actor package %s."), *PackageName);
				}
			}
			else
			{
				bSaved = Context.Services.SaveAsset(Target->GetPathName(), false, SaveError);
			}
			Row->SetBoolField(TEXT("saved"), bSaved);
			Row->SetStringField(TEXT("error"), SaveError);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			if (!bSaved)
			{
				Receipt->SetArrayField(TEXT("save_receipts"), Rows);
				Error = SaveError.IsEmpty() ? FString::Printf(TEXT("Failed to save %s."), *Target->GetPathName()) : SaveError;
				return false;
			}
		}
		Receipt->SetArrayField(TEXT("save_receipts"), Rows);
		return true;
	}

	static FString CheckpointDirectory()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("MeshTerrainRecovery"));
	}

	static FString ManifestPath(const FString& CheckpointId)
	{
		return FPaths::Combine(CheckpointDirectory(), CheckpointId + TEXT(".json"));
	}

	static bool RestoreRoots(const FJsonObjectRef& Arguments, TArray<FString>& OutRoots, FString& Error)
	{
		OutRoots = StringArray(Arguments, TEXT("allowed_restore_roots"));
		if (OutRoots.IsEmpty())
		{
			OutRoots.Add(TEXT("/Game/SOMOLMCP/Disposable"));
		}
		for (FString& Root : OutRoots)
		{
			if (!NormalizeContentRoot(Root))
			{
				Error = FString::Printf(TEXT("Invalid allowed_restore_roots entry: %s"), *Root);
				return false;
			}
		}
		OutRoots.Sort();
		for (int32 Index = OutRoots.Num() - 1; Index > 0; --Index)
		{
			if (OutRoots[Index] == OutRoots[Index - 1]) OutRoots.RemoveAt(Index);
		}
		return true;
	}

	static bool IsAllowedRestoreTarget(const FString& ObjectPath, const TArray<FString>& Roots)
	{
		for (const FString& Root : Roots)
		{
			if (IsUnderContentRoot(ObjectPath, Root)) return true;
		}
		return false;
	}

	static FString SnapshotString(const FJsonObjectRef& Value, const TCHAR* Field)
	{
		FString Result;
		Value->TryGetStringField(Field, Result);
		return Result;
	}

	static FString SafeAssetToken(const FString& ObjectPath)
	{
		FString Token = ObjectPath;
		Token.ReplaceInline(TEXT("/Game/"), TEXT(""));
		Token.ReplaceInline(TEXT("."), TEXT("_"));
		Token.ReplaceInline(TEXT("/"), TEXT("_"));
		Token.ReplaceInline(TEXT(":"), TEXT("_"));
		return Token.Left(96);
	}

	static bool ExecuteCheckpointCreate(const FSololmcpToolExecutionContext& Context, const FJsonObjectRef& Arguments,
		const TArray<UObject*>& Targets, FJsonObjectRef& Out, FJsonObjectRef& Receipt, FString& Summary, FString& Error)
	{
		if (Targets.IsEmpty())
		{
			return FailClosed(Out, Error, TEXT("target_binding_required"), TEXT("At least one persistent terrain asset is required for a recovery checkpoint."));
		}
		FString CheckpointId;
		Arguments->TryGetStringField(TEXT("checkpoint_id"), CheckpointId);
		if (CheckpointId.IsEmpty())
		{
			CheckpointId = FString::Printf(TEXT("mesh_terrain_%s_%s"),
				*FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
		}
		if (!IsSafeIdentifier(CheckpointId))
		{
			return FailClosed(Out, Error, TEXT("checkpoint_id_invalid"), TEXT("checkpoint_id contains unsupported characters."));
		}
		if (IFileManager::Get().FileExists(*ManifestPath(CheckpointId)))
		{
			return FailClosed(Out, Error, TEXT("checkpoint_manifest_exists"), TEXT("Refusing to overwrite an existing checkpoint manifest."));
		}

		FString BackupRoot = TEXT("/Game/SOMOLMCP/Recovery/MeshTerrain");
		Arguments->TryGetStringField(TEXT("backup_root"), BackupRoot);
		if (!NormalizeContentRoot(BackupRoot) || !IsUnderContentRoot(BackupRoot, TEXT("/Game/SOMOLMCP/Recovery/MeshTerrain")))
		{
			return FailClosed(Out, Error, TEXT("invalid_backup_root"),
				TEXT("backup_root must remain under /Game/SOMOLMCP/Recovery/MeshTerrain."));
		}
		TArray<FString> AllowedRoots;
		if (!RestoreRoots(Arguments, AllowedRoots, Error))
		{
			return FailClosed(Out, Error, TEXT("invalid_restore_roots"), Error);
		}

		FJsonObjectRef Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(TEXT("schema"), TEXT("somolmcp.mesh_terrain_recovery_manifest.v2"));
		Manifest->SetStringField(TEXT("manifest_kind"), TEXT("mesh_terrain_recovery_checkpoint"));
		Manifest->SetStringField(TEXT("checkpoint_id"), CheckpointId);
		Manifest->SetStringField(TEXT("created_at"), FDateTime::UtcNow().ToIso8601());
		Manifest->SetStringField(TEXT("project_dir"), CanonicalProjectDir());
		Manifest->SetStringField(TEXT("project_file"), CanonicalProjectFile());
		Manifest->SetStringField(TEXT("backup_root"), BackupRoot);
		Manifest->SetArrayField(TEXT("allowed_restore_roots"), ToJson(AllowedRoots));
		FString SessionId;
		if (Arguments->TryGetStringField(TEXT("session_id"), SessionId) && !SessionId.IsEmpty())
		{
			Manifest->SetStringField(TEXT("session_id"), SessionId);
			Manifest->SetStringField(TEXT("session_manifest_path"), SessionManifestPath(SessionId));
		}
		TArray<TSharedPtr<FJsonValue>> Backups;
		int32 Sequence = 0;
		for (UObject* Target : Targets)
		{
			const FString SourcePath = Target->GetPathName();
			if (!FPackageName::IsValidObjectPath(SourcePath))
			{
				return FailClosed(Out, Error, TEXT("non_persistent_recovery_target"),
					FString::Printf(TEXT("Recovery target is not a persistent asset object path: %s"), *SourcePath));
			}
			if (!IsAllowedRestoreTarget(SourcePath, AllowedRoots))
			{
				return FailClosed(Out, Error, TEXT("recovery_target_outside_allowed_roots"),
					FString::Printf(TEXT("Recovery target is outside Disposable/explicit allowed roots: %s"), *SourcePath));
			}
			const FJsonObjectRef SourceSnapshot = Snapshot(Target);
			const FString BackupPath = FString::Printf(TEXT("%s/%s/%04d_%s"), *BackupRoot, *CheckpointId, ++Sequence, *SafeAssetToken(SourcePath));
			FString DuplicateError;
			UObject* Backup = Context.Services.DuplicateAsset(SourcePath, BackupPath, DuplicateError);
			if (!Backup)
			{
				return FailClosed(Out, Error, TEXT("checkpoint_duplicate_failed"),
					DuplicateError.IsEmpty() ? FString::Printf(TEXT("Failed to duplicate %s to %s."), *SourcePath, *BackupPath) : DuplicateError);
			}
			FString SaveError;
			if (!Context.Services.SaveAsset(Backup->GetPathName(), false, SaveError))
			{
				return FailClosed(Out, Error, TEXT("checkpoint_save_failed"), SaveError);
			}
			const FJsonObjectRef BackupSnapshot = Snapshot(Backup);
			if (SnapshotString(SourceSnapshot, TEXT("property_crc32")) != SnapshotString(BackupSnapshot, TEXT("property_crc32"))
				|| SnapshotString(SourceSnapshot, TEXT("class_path")) != SnapshotString(BackupSnapshot, TEXT("class_path")))
			{
				return FailClosed(Out, Error, TEXT("checkpoint_backup_readback_mismatch"),
					FString::Printf(TEXT("Backup readback did not match source %s."), *SourcePath));
			}
			FJsonObjectRef Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("sequence"), Sequence);
			Row->SetStringField(TEXT("source_path"), SourcePath);
			Row->SetStringField(TEXT("backup_path"), Backup->GetPathName());
			Row->SetObjectField(TEXT("source_snapshot"), SourceSnapshot);
			Row->SetObjectField(TEXT("backup_readback"), BackupSnapshot);
			Row->SetStringField(TEXT("expected_class_path"), SnapshotString(SourceSnapshot, TEXT("class_path")));
			Row->SetStringField(TEXT("expected_property_crc32"), SnapshotString(BackupSnapshot, TEXT("property_crc32")));
			Backups.Add(MakeShared<FJsonValueObject>(Row));
		}
		Manifest->SetArrayField(TEXT("backups"), Backups);
		IFileManager::Get().MakeDirectory(*CheckpointDirectory(), true);
		if (!FFileHelper::SaveStringToFile(JsonText(Manifest), *ManifestPath(CheckpointId), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return FailClosed(Out, Error, TEXT("checkpoint_manifest_write_failed"), TEXT("Failed to persist the project-local checkpoint manifest."));
		}
		Receipt->SetStringField(TEXT("checkpoint_id"), CheckpointId);
		Receipt->SetStringField(TEXT("manifest_path"), ManifestPath(CheckpointId));
		Receipt->SetNumberField(TEXT("backup_count"), Backups.Num());
		Receipt->SetArrayField(TEXT("backups"), Backups);
		Out->SetStringField(TEXT("checkpoint_id"), CheckpointId);
		Out->SetStringField(TEXT("manifest_path"), ManifestPath(CheckpointId));
		Summary = FString::Printf(TEXT("Created Mesh Terrain recovery checkpoint %s with %d persistent backup asset(s)."), *CheckpointId, Backups.Num());
		return true;
	}

	struct FRestorePlanRow
	{
		int32 Sequence = 0;
		FString SourcePath;
		FString BackupPath;
		FString ExpectedClassPath;
		FString ExpectedCrc;
		FString OriginalClassPath;
		FString OriginalCrc;
		FString StagePath;
		FString RollbackPath;
		FString QuarantinePath;
		bool bHadSource = false;
		bool bCommitted = false;
	};

	static bool ExecuteCheckpointRestore(const FSololmcpToolExecutionContext& Context, const FJsonObjectRef& Arguments,
		FJsonObjectRef& Out, FJsonObjectRef& Receipt, FString& Summary, FString& Error)
	{
		FString CheckpointId;
		if (!Arguments->TryGetStringField(TEXT("checkpoint_id"), CheckpointId) || CheckpointId.IsEmpty())
		{
			return FailClosed(Out, Error, TEXT("checkpoint_id_required"), TEXT("checkpoint_id is required."));
		}
		if (!IsSafeIdentifier(CheckpointId))
		{
			return FailClosed(Out, Error, TEXT("checkpoint_id_invalid"), TEXT("checkpoint_id contains unsupported characters."));
		}
		bool bConfirmOverwrite = false;
		Arguments->TryGetBoolField(TEXT("confirm_overwrite"), bConfirmOverwrite);
		if (!bConfirmOverwrite)
		{
			return FailClosed(Out, Error, TEXT("restore_overwrite_confirmation_required"),
				TEXT("confirm_overwrite=true is required because restore replaces current terrain assets."));
		}
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *ManifestPath(CheckpointId)))
		{
			return FailClosed(Out, Error, TEXT("checkpoint_manifest_missing"),
				FString::Printf(TEXT("Checkpoint manifest was not found: %s"), *ManifestPath(CheckpointId)));
		}
		TSharedPtr<FJsonObject> Manifest;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Manifest) || !Manifest.IsValid())
		{
			return FailClosed(Out, Error, TEXT("checkpoint_manifest_invalid"), TEXT("Checkpoint manifest is not valid JSON."));
		}
		FString ManifestSchema;
		FString ManifestKind;
		FString ManifestCheckpointId;
		FString ManifestProjectDir;
		FString ManifestProjectFile;
		FString BackupRoot;
		Manifest->TryGetStringField(TEXT("schema"), ManifestSchema);
		Manifest->TryGetStringField(TEXT("manifest_kind"), ManifestKind);
		Manifest->TryGetStringField(TEXT("checkpoint_id"), ManifestCheckpointId);
		Manifest->TryGetStringField(TEXT("project_dir"), ManifestProjectDir);
		Manifest->TryGetStringField(TEXT("project_file"), ManifestProjectFile);
		Manifest->TryGetStringField(TEXT("backup_root"), BackupRoot);
		if (ManifestSchema != TEXT("somolmcp.mesh_terrain_recovery_manifest.v2")
			|| ManifestKind != TEXT("mesh_terrain_recovery_checkpoint") || ManifestCheckpointId != CheckpointId
			|| ManifestProjectDir != CanonicalProjectDir() || ManifestProjectFile != CanonicalProjectFile()
			|| !NormalizeContentRoot(BackupRoot)
			|| !IsUnderContentRoot(BackupRoot, TEXT("/Game/SOMOLMCP/Recovery/MeshTerrain")))
		{
			return FailClosed(Out, Error, TEXT("checkpoint_manifest_binding_invalid"),
				TEXT("Checkpoint manifest schema, kind, id, project binding, or backup root did not validate."));
		}
		TArray<FString> ManifestAllowedRoots = StringArray(Manifest.ToSharedRef(), TEXT("allowed_restore_roots"));
		for (FString& Root : ManifestAllowedRoots)
		{
			if (!NormalizeContentRoot(Root))
			{
				return FailClosed(Out, Error, TEXT("checkpoint_manifest_roots_invalid"), TEXT("Checkpoint manifest contains an invalid allowed restore root."));
			}
		}
		TArray<FString> RequestedAllowedRoots;
		if (!RestoreRoots(Arguments, RequestedAllowedRoots, Error))
		{
			return FailClosed(Out, Error, TEXT("invalid_restore_roots"), Error);
		}
		const TArray<TSharedPtr<FJsonValue>>* Backups = nullptr;
		if (!Manifest->TryGetArrayField(TEXT("backups"), Backups) || !Backups || Backups->IsEmpty())
		{
			return FailClosed(Out, Error, TEXT("checkpoint_empty"), TEXT("Checkpoint contains no backup assets."));
		}

		TArray<FRestorePlanRow> Plan;
		TSet<int32> Sequences;
		TSet<FString> Sources;
		const FString RestoreId = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
		// Phase 0 validates every row and every backup before creating any stage or rollback asset.
		for (const TSharedPtr<FJsonValue>& Value : *Backups)
		{
			const TSharedPtr<FJsonObject>* Row = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Row) || !Row || !Row->IsValid())
			{
				return FailClosed(Out, Error, TEXT("checkpoint_row_invalid"), TEXT("Checkpoint contains a malformed backup row."));
			}
			FString SourcePath;
			FString BackupPath;
			FString ExpectedClassPath;
			FString ExpectedCrc;
			double SequenceNumber = 0.0;
			(*Row)->TryGetStringField(TEXT("source_path"), SourcePath);
			(*Row)->TryGetStringField(TEXT("backup_path"), BackupPath);
			(*Row)->TryGetStringField(TEXT("expected_class_path"), ExpectedClassPath);
			(*Row)->TryGetStringField(TEXT("expected_property_crc32"), ExpectedCrc);
			(*Row)->TryGetNumberField(TEXT("sequence"), SequenceNumber);
			const int32 Sequence = static_cast<int32>(SequenceNumber);
			if (!FPackageName::IsValidObjectPath(SourcePath) || !FPackageName::IsValidObjectPath(BackupPath)
				|| Sequence <= 0 || Sequences.Contains(Sequence) || Sources.Contains(SourcePath)
				|| !IsUnderContentRoot(BackupPath, BackupRoot)
				|| !IsAllowedRestoreTarget(SourcePath, ManifestAllowedRoots)
				|| !IsAllowedRestoreTarget(SourcePath, RequestedAllowedRoots))
			{
				return FailClosed(Out, Error, TEXT("checkpoint_row_binding_invalid"),
					FString::Printf(TEXT("Checkpoint row is malformed or target %s is outside Disposable/manifest/caller allowed roots."), *SourcePath));
			}
			FString LoadError;
			UObject* Backup = Context.Services.LoadAsset(BackupPath, LoadError);
			if (!Backup || !Context.Services.AssetExists(BackupPath))
			{
				return FailClosed(Out, Error, TEXT("checkpoint_backup_missing"),
					FString::Printf(TEXT("Checkpoint backup is unavailable for %s."), *SourcePath));
			}
			const FJsonObjectRef BackupSnapshot = Snapshot(Backup);
			if (ExpectedClassPath.IsEmpty() || ExpectedCrc.IsEmpty()
				|| SnapshotString(BackupSnapshot, TEXT("class_path")) != ExpectedClassPath
				|| SnapshotString(BackupSnapshot, TEXT("property_crc32")) != ExpectedCrc)
			{
				return FailClosed(Out, Error, TEXT("checkpoint_backup_fingerprint_mismatch"),
					FString::Printf(TEXT("Checkpoint backup fingerprint mismatch for %s."), *SourcePath));
			}
			FRestorePlanRow PlanRow;
			PlanRow.Sequence = Sequence;
			PlanRow.SourcePath = SourcePath;
			PlanRow.BackupPath = BackupPath;
			PlanRow.ExpectedClassPath = ExpectedClassPath;
			PlanRow.ExpectedCrc = ExpectedCrc;
			PlanRow.bHadSource = Context.Services.AssetExists(SourcePath);
			if (PlanRow.bHadSource)
			{
				FString SourceLoadError;
				UObject* CurrentSource = Context.Services.LoadAsset(SourcePath, SourceLoadError);
				if (!CurrentSource)
				{
					return FailClosed(Out, Error, TEXT("restore_current_target_unreadable"),
						FString::Printf(TEXT("Current restore target could not be read before rollback staging: %s"), *SourcePath));
				}
				const FJsonObjectRef CurrentSnapshot = Snapshot(CurrentSource);
				PlanRow.OriginalClassPath = SnapshotString(CurrentSnapshot, TEXT("class_path"));
				PlanRow.OriginalCrc = SnapshotString(CurrentSnapshot, TEXT("property_crc32"));
			}
			const FString Token = SafeAssetToken(SourcePath);
			PlanRow.StagePath = FString::Printf(TEXT("/Game/SOMOLMCP/Disposable/MeshTerrainRestoreStage/%s/%s/%04d_%s"), *CheckpointId, *RestoreId, Sequence, *Token);
			PlanRow.RollbackPath = FString::Printf(TEXT("/Game/SOMOLMCP/Disposable/MeshTerrainRestoreRollback/%s/%s/%04d_%s"), *CheckpointId, *RestoreId, Sequence, *Token);
			PlanRow.QuarantinePath = FString::Printf(TEXT("/Game/SOMOLMCP/Disposable/MeshTerrainRestoreQuarantine/%s/%s/%04d_%s"), *CheckpointId, *RestoreId, Sequence, *Token);
			Sequences.Add(Sequence);
			Sources.Add(SourcePath);
			Plan.Add(MoveTemp(PlanRow));
		}
		Plan.Sort([](const FRestorePlanRow& A, const FRestorePlanRow& B) { return A.Sequence < B.Sequence; });

		// Phase 1 stages every backup and creates every rollback copy before any source path is moved.
		for (FRestorePlanRow& Row : Plan)
		{
			FString StageError;
			UObject* Stage = Context.Services.DuplicateAsset(Row.BackupPath, Row.StagePath, StageError);
			if (!Stage || !Context.Services.SaveAsset(Stage->GetPathName(), false, StageError))
			{
				return FailClosed(Out, Error, TEXT("restore_stage_failed"), StageError.IsEmpty() ? TEXT("Failed to stage a validated checkpoint backup.") : StageError);
			}
			Row.StagePath = Stage->GetPathName();
			const FJsonObjectRef StageSnapshot = Snapshot(Stage);
			if (SnapshotString(StageSnapshot, TEXT("class_path")) != Row.ExpectedClassPath
				|| SnapshotString(StageSnapshot, TEXT("property_crc32")) != Row.ExpectedCrc)
			{
				return FailClosed(Out, Error, TEXT("restore_stage_readback_mismatch"), TEXT("Staged checkpoint copy failed class/property fingerprint validation."));
			}
			if (Row.bHadSource)
			{
				FString RollbackError;
				UObject* Rollback = Context.Services.DuplicateAsset(Row.SourcePath, Row.RollbackPath, RollbackError);
				if (!Rollback || !Context.Services.SaveAsset(Rollback->GetPathName(), false, RollbackError))
				{
					return FailClosed(Out, Error, TEXT("restore_rollback_stage_failed"), RollbackError.IsEmpty() ? TEXT("Failed to stage current target rollback copy.") : RollbackError);
				}
				Row.RollbackPath = Rollback->GetPathName();
				const FJsonObjectRef RollbackSnapshot = Snapshot(Rollback);
				if (SnapshotString(RollbackSnapshot, TEXT("class_path")) != Row.OriginalClassPath
					|| SnapshotString(RollbackSnapshot, TEXT("property_crc32")) != Row.OriginalCrc)
				{
					return FailClosed(Out, Error, TEXT("restore_rollback_stage_readback_mismatch"),
						TEXT("Current target rollback copy failed class/property fingerprint validation."));
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> RestoreRows;
		FString CommitError;
		for (FRestorePlanRow& Row : Plan)
		{
			if (Row.bHadSource && !Context.Services.RenameAsset(Row.SourcePath, Row.QuarantinePath, CommitError)) break;
			if (!Context.Services.RenameAsset(Row.StagePath, Row.SourcePath, CommitError)) break;
			FString SaveError;
			UObject* Restored = Context.Services.LoadAsset(Row.SourcePath, SaveError);
			if (!Restored || !Context.Services.SaveAsset(Row.SourcePath, false, SaveError))
			{
				CommitError = SaveError.IsEmpty() ? TEXT("Restored target could not be loaded and saved.") : SaveError;
				break;
			}
			const FJsonObjectRef PostReadback = Snapshot(Restored);
			if (SnapshotString(PostReadback, TEXT("class_path")) != Row.ExpectedClassPath
				|| SnapshotString(PostReadback, TEXT("property_crc32")) != Row.ExpectedCrc)
			{
				CommitError = FString::Printf(TEXT("Post-restore fingerprint mismatch for %s."), *Row.SourcePath);
				break;
			}
			Row.bCommitted = true;
			FJsonObjectRef RestoreRow = MakeShared<FJsonObject>();
			RestoreRow->SetNumberField(TEXT("sequence"), Row.Sequence);
			RestoreRow->SetStringField(TEXT("source_path"), Row.SourcePath);
			RestoreRow->SetStringField(TEXT("backup_path"), Row.BackupPath);
			RestoreRow->SetStringField(TEXT("rollback_path"), Row.RollbackPath);
			RestoreRow->SetStringField(TEXT("quarantine_path"), Row.QuarantinePath);
			RestoreRow->SetObjectField(TEXT("post_restore_readback"), PostReadback);
			RestoreRows.Add(MakeShared<FJsonValueObject>(RestoreRow));
		}

		if (!CommitError.IsEmpty())
		{
			bool bRollbackOk = true;
			TArray<TSharedPtr<FJsonValue>> RollbackRows;
			for (FRestorePlanRow& Row : Plan)
			{
				FJsonObjectRef RollbackRow = MakeShared<FJsonObject>();
				RollbackRow->SetStringField(TEXT("source_path"), Row.SourcePath);
				FString RowError;
				bool bRowRollbackOk = true;
				if (Context.Services.AssetExists(Row.SourcePath))
				{
					const FString FailedPath = Row.QuarantinePath + TEXT("_failed_restore");
					bRowRollbackOk = Context.Services.RenameAsset(Row.SourcePath, FailedPath, RowError);
				}
				if (Row.bHadSource && !Context.Services.AssetExists(Row.SourcePath))
				{
					UObject* RolledBack = Context.Services.DuplicateAsset(Row.RollbackPath, Row.SourcePath, RowError);
					bRowRollbackOk = RolledBack && Context.Services.SaveAsset(Row.SourcePath, false, RowError) && bRowRollbackOk;
				}
				if (Row.bHadSource && bRowRollbackOk)
				{
					FString ReadbackError;
					UObject* RolledBack = Context.Services.LoadAsset(Row.SourcePath, ReadbackError);
					const FJsonObjectRef Readback = Snapshot(RolledBack);
					bRowRollbackOk = RolledBack
						&& SnapshotString(Readback, TEXT("class_path")) == Row.OriginalClassPath
						&& SnapshotString(Readback, TEXT("property_crc32")) == Row.OriginalCrc;
					if (!bRowRollbackOk && RowError.IsEmpty()) RowError = TEXT("Rollback target fingerprint verification failed.");
				}
				if (!Row.bHadSource) bRowRollbackOk = !Context.Services.AssetExists(Row.SourcePath) && bRowRollbackOk;
				bRollbackOk = bRollbackOk && bRowRollbackOk;
				RollbackRow->SetBoolField(TEXT("rolled_back"), bRowRollbackOk);
				RollbackRow->SetStringField(TEXT("error"), RowError);
				RollbackRows.Add(MakeShared<FJsonValueObject>(RollbackRow));
			}
			Out->SetArrayField(TEXT("rollback_rows"), RollbackRows);
			Out->SetBoolField(TEXT("rollback_verified"), bRollbackOk);
			return FailClosed(Out, Error, TEXT("restore_commit_failed_rolled_back"),
				FString::Printf(TEXT("Two-phase restore commit failed and rollback was attempted (verified=%s): %s"), bRollbackOk ? TEXT("true") : TEXT("false"), *CommitError));
		}
		Receipt->SetStringField(TEXT("checkpoint_id"), CheckpointId);
		Receipt->SetStringField(TEXT("restore_protocol"), TEXT("validate_all_then_stage_and_rollback_then_commit"));
		Receipt->SetBoolField(TEXT("all_backups_prevalidated"), true);
		Receipt->SetBoolField(TEXT("rollback_staged_before_commit"), true);
		Receipt->SetArrayField(TEXT("restore_rows"), RestoreRows);
		Receipt->SetNumberField(TEXT("restored_count"), RestoreRows.Num());
		Out->SetArrayField(TEXT("restored_assets"), RestoreRows);
		Summary = FString::Printf(TEXT("Restored %d Mesh Terrain asset(s) from checkpoint %s."), RestoreRows.Num(), *CheckpointId);
		return true;
	}

	static bool HasExplicitTargetBinding(const FJsonObjectRef& Arguments)
	{
		for (const TCHAR* Field : {TEXT("target_paths"), TEXT("target_actor_ids"), TEXT("target_asset"),
			TEXT("mesh_partition_asset"), TEXT("landscape_actor"), TEXT("mesh_asset")})
		{
			if (Arguments->HasField(Field)) return true;
		}
		return false;
	}

	static bool BindSessionTargets(const FSololmcpToolExecutionContext& Context, const FJsonObjectRef& Arguments,
		TArray<UObject*>& InOutTargets, FSculptSession& OutSession, FString& OutSessionId, FJsonObjectRef& Out, FString& Error)
	{
		if (!Arguments->TryGetStringField(TEXT("session_id"), OutSessionId) || OutSessionId.IsEmpty())
		{
			return true;
		}
		if (!LoadSession(OutSessionId, OutSession, Error))
		{
			return false;
		}
		TArray<UObject*> SessionTargets;
		for (const FString& TargetPath : OutSession.TargetPaths)
		{
			UObject* Target = ResolveOne(Context, TargetPath);
			if (!Target || Target->GetPathName() != TargetPath)
			{
				Error = FString::Printf(TEXT("Session target could not be resolved exactly: %s"), *TargetPath);
				return false;
			}
			SessionTargets.AddUnique(Target);
		}
		if (SessionTargets.Num() != OutSession.TargetPaths.Num())
		{
			Error = TEXT("Session target readback count did not match its manifest.");
			return false;
		}
		if (HasExplicitTargetBinding(Arguments))
		{
			TSet<FString> ExplicitPaths;
			for (UObject* Target : InOutTargets) ExplicitPaths.Add(Target->GetPathName());
			TSet<FString> SessionPaths;
			for (const FString& Path : OutSession.TargetPaths) SessionPaths.Add(Path);
			if (ExplicitPaths.Num() != SessionPaths.Num())
			{
				Error = TEXT("Explicit targets do not match the session manifest target set.");
				return false;
			}
			for (const FString& Path : SessionPaths)
			{
				if (!ExplicitPaths.Contains(Path))
				{
					Error = TEXT("Explicit targets do not match the session manifest target set.");
					return false;
				}
			}
		}
		InOutTargets = MoveTemp(SessionTargets);
		Out->SetStringField(TEXT("session_id"), OutSessionId);
		Out->SetStringField(TEXT("session_manifest_path"), SessionManifestPath(OutSessionId));
		Out->SetBoolField(TEXT("session_restored_from_manifest_or_cache"), true);
		Out->SetArrayField(TEXT("session_targets"), ToJson(OutSession.TargetPaths));
		return true;
	}

	static bool ConsumeSession(FSculptSession& Session, const FString& Operation, FJsonObjectRef& Out, FString& Error)
	{
		Session.LastOperation = Operation;
		Session.UpdatedAt = FDateTime::UtcNow().ToIso8601();
		++Session.ConsumedOperationCount;
		if (!PersistSession(Session, Error)) return false;
		{
			FScopeLock Lock(&SessionLock);
			Sessions.Add(Session.Id, Session);
		}
		Out->SetBoolField(TEXT("session_consumed"), true);
		Out->SetNumberField(TEXT("session_consumed_operation_count"), Session.ConsumedOperationCount);
		return true;
	}

	static bool HasReflectionPatch(const FJsonObjectRef& Arguments)
	{
		if (Arguments->HasField(TEXT("properties"))) return true;
		const TSharedPtr<FJsonObject>* Extra = nullptr;
		return Arguments->TryGetObjectField(TEXT("operation_args"), Extra) && Extra && Extra->IsValid()
			&& (*Extra)->HasField(TEXT("properties"));
	}

	static bool ValidateReadbackResult(const FJsonObjectRef& Result, const FString& ExpectedTarget)
	{
		const TSharedPtr<FJsonObject>* Readback = nullptr;
		if (Result->TryGetObjectField(TEXT("readback_receipt"), Readback) && Readback && Readback->IsValid())
		{
			bool bResolved = false;
			FString ObjectPath;
			(*Readback)->TryGetBoolField(TEXT("resolved"), bResolved);
			(*Readback)->TryGetStringField(TEXT("object_path"), ObjectPath);
			return bResolved && ObjectPath == ExpectedTarget;
		}
		return false;
	}

	static FJsonObjectRef TypedOperationArgumentsSchema()
	{
		using SB = FSololmcpSchemaBuilder;
		return SB::Object({
			{TEXT("target_asset"), SB::String()}, {TEXT("mesh_partition_asset"), SB::String()},
			{TEXT("target_level"), SB::String()}, {TEXT("source_layer"), SB::String()},
			{TEXT("target_layer"), SB::String()}, {TEXT("layer_name"), SB::String()},
			{TEXT("new_layer_name"), SB::String()}, {TEXT("from_index"), SB::Integer(TEXT(""), 0, 4095)},
			{TEXT("to_index"), SB::Integer(TEXT(""), 0, 4095)}, {TEXT("stamp_asset"), SB::String()},
			{TEXT("hole_type"), SB::String(TEXT(""), {TEXT("hole"), TEXT("cave"), TEXT("tunnel"), TEXT("overhang"), TEXT("fill")})},
			{TEXT("erosion_mode"), SB::String(TEXT(""), {TEXT("thermal"), TEXT("hydraulic"), TEXT("slope")})},
			{TEXT("strength"), SB::Number(TEXT(""), 0.0, 1.0)}, {TEXT("radius_cm"), SB::Number(TEXT(""), 1.0, 1000000.0)},
			{TEXT("iterations"), SB::Integer(TEXT(""), 1, 1024)}, {TEXT("save"), SB::Boolean()}
		}, {}, TEXT("Closed typed arguments forwarded to the native dependency tool."), false);
	}

	static void CloseObjectSchemas(const FJsonObjectRef& Schema)
	{
		FString Type;
		if (Schema->TryGetStringField(TEXT("type"), Type) && Type == TEXT("object"))
		{
			Schema->SetBoolField(TEXT("additionalProperties"), false);
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Schema->Values)
		{
			const TSharedPtr<FJsonObject>* ChildObject = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetObject(ChildObject) && ChildObject && ChildObject->IsValid())
			{
				CloseObjectSchemas((*ChildObject).ToSharedRef());
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* ChildArray = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetArray(ChildArray) && ChildArray)
			{
				for (const TSharedPtr<FJsonValue>& Child : *ChildArray)
				{
					if (Child.IsValid() && Child->TryGetObject(ChildObject) && ChildObject && ChildObject->IsValid())
					{
						CloseObjectSchemas((*ChildObject).ToSharedRef());
					}
				}
			}
		}
	}

	static FJsonObjectRef Schema()
	{
		using SB = FSololmcpSchemaBuilder;
		return SB::Object({
			{TEXT("session_id"), SB::String(TEXT("Existing or requested multi-target sculpt session id."))},
			{TEXT("target_paths"), SB::Array(SB::String(), TEXT("Persistent asset object paths or actor labels/paths."), 1, 256, true)},
			{TEXT("target_actor_ids"), SB::Array(SB::String(), TEXT("Actor labels or object paths to select."), 1, 256, true)},
			{TEXT("target_asset"), SB::String(TEXT("Primary Mesh Terrain or MeshPartition asset object path."))},
			{TEXT("mesh_partition_asset"), SB::String(TEXT("MeshPartition target object path."))},
			{TEXT("landscape_actor"), SB::String(TEXT("Landscape actor label or object path for diff/readback."))},
			{TEXT("mesh_asset"), SB::String(TEXT("Mesh Terrain asset or actor for diff/readback."))},
			{TEXT("source_layer"), SB::String(TEXT("Source sculpt layer name."))},
			{TEXT("target_layer"), SB::String(TEXT("Destination sculpt layer name."))},
			{TEXT("layer_name"), SB::String(TEXT("Sculpt layer name."))},
			{TEXT("new_layer_name"), SB::String(TEXT("New name for a duplicated sculpt layer."))},
			{TEXT("from_index"), SB::Integer(TEXT("Current sculpt layer index."), 0, 4095)},
			{TEXT("to_index"), SB::Integer(TEXT("Destination sculpt layer index."), 0, 4095)},
			{TEXT("stamp_asset"), SB::String(TEXT("Texture, mesh, or patch asset used as the terrain stamp."))},
			{TEXT("hole_type"), SB::String(TEXT("Hole topology operation."), {TEXT("hole"), TEXT("cave"), TEXT("tunnel"), TEXT("overhang"), TEXT("fill")})},
			{TEXT("erosion_mode"), SB::String(TEXT("Erosion model."), {TEXT("thermal"), TEXT("hydraulic"), TEXT("slope")})},
			{TEXT("strength"), SB::Number(TEXT("Normalized operation strength."), 0.0, 1.0)},
			{TEXT("radius_cm"), SB::Number(TEXT("Bounded operation radius in centimeters."), 1.0, 1000000.0)},
			{TEXT("iterations"), SB::Integer(TEXT("Bounded erosion/repair iteration count."), 1, 1024)},
			{TEXT("operation_args"), TypedOperationArgumentsSchema()},
			{TEXT("target_level"), SB::String(TEXT("Current World Partition level path."))},
			{TEXT("hlod_layer_index"), SB::Integer(TEXT("HLOD layer index."), 0, 255)},
			{TEXT("checkpoint_id"), SB::String(TEXT("Persistent recovery checkpoint id."), {}, 1, 128)},
			{TEXT("checkpoint_label"), SB::String(TEXT("Optional human-readable checkpoint label."), {}, 0, 256)},
			{TEXT("backup_root"), SB::String(TEXT("Project content backup root; default /Game/SOMOLMCP/Recovery/MeshTerrain."))},
			{TEXT("allowed_restore_roots"), SB::Array(SB::String(TEXT("Explicit /Game root authorized for checkpoint create/restore.")), TEXT("Defaults to /Game/SOMOLMCP/Disposable; non-disposable restore roots must be repeated by the restore caller."), 1, 32, true)},
			{TEXT("confirm_overwrite"), SB::Boolean(TEXT("Required true for destructive checkpoint restoration."))},
			{TEXT("save"), SB::Boolean(TEXT("Save affected persistent assets; defaults true."))},
			{TEXT("include_dependencies"), SB::Boolean(TEXT("Include asset dependency readback."))}
		}, {}, TEXT("UE 5.8 native Mesh Terrain P4 closure contract."), false);
	}

	static bool Execute(
		FSololmcpToolRegistry& Registry,
		const FSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const FJsonObjectRef& Arguments,
		FJsonObjectRef& Out,
		FString& Summary,
		FString& Error)
	{
		Out->SetStringField(TEXT("tool"), Spec.Name);
		Out->SetStringField(TEXT("implementation"), TEXT("ue58_native_mesh_terrain_completion_p4"));
		Out->SetStringField(TEXT("execution_backend"), TEXT("native_cpp_queue"));
		Out->SetBoolField(TEXT("python_backend"), false);
		Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());

#if ENGINE_MAJOR_VERSION != 5 || ENGINE_MINOR_VERSION < 8
		FString VersionExitPrevious;
		FString VersionExitCurrent;
		FString VersionExitError;
		const bool bVersionExitOk = TerrainModeGuard::ForceSelectionMode(VersionExitPrevious, VersionExitCurrent, VersionExitError);
		Out->SetBoolField(TEXT("select_mode_restored"), bVersionExitOk);
		Out->SetStringField(TEXT("select_mode_after_operation"), VersionExitCurrent);
		if (!bVersionExitOk) return FailClosed(Out, Error, TEXT("select_mode_exit_failed"), VersionExitError);
		return FailClosed(Out, Error, TEXT("requires_ue_5_8"), TEXT("Mesh Terrain P4 closure tools require Unreal Engine 5.8 or later."));
#else
		TerrainModeGuard::FSelectionScope ModeGuard;
		FString EntryError;
		const bool bEntryOk = ModeGuard.Begin(EntryError);
		ModeGuard.Attach(Out);

		TArray<UObject*> Targets = ResolveTargets(Context, Arguments);
		FSculptSession BoundSession;
		FString BoundSessionId;
		FString BindingError;
		const bool bSessionBindingOk = Spec.Operation == EOperation::SessionBegin
			|| BindSessionTargets(Context, Arguments, Targets, BoundSession, BoundSessionId, Out, BindingError);
		FJsonObjectRef Receipt = BeginReceipt(Spec, Targets);
		Out->SetObjectField(TEXT("receipt"), Receipt);
		const double StartSeconds = FPlatformTime::Seconds();
		bool bOk = false;

		if (!bEntryOk)
		{
			bOk = FailClosed(Out, Error, TEXT("select_mode_entry_failed"), EntryError);
		}
		else if (!bSessionBindingOk)
		{
			bOk = FailClosed(Out, Error, TEXT("session_binding_failed"), BindingError);
		}
		else
		{
			const bool bCoreWrite = Spec.Operation == EOperation::LayerMerge || Spec.Operation == EOperation::LayerDuplicate
				|| Spec.Operation == EOperation::LayerReorder || Spec.Operation == EOperation::Stamp
				|| Spec.Operation == EOperation::Erosion || Spec.Operation == EOperation::HoleCaveOverhang
				|| Spec.Operation == EOperation::SeamRepair || Spec.Operation == EOperation::WorldPartitionCommit;
			if (bCoreWrite && HasReflectionPatch(Arguments))
			{
				bOk = FailClosed(Out, Error, TEXT("reflection_patch_not_authoritative_write"),
					TEXT("Reflected properties are not accepted as stamp, erosion, hole, seam, sculpt-layer, or World Partition commit evidence."));
			}
			else
			{
				bOk = [&]() -> bool
				{
				switch (Spec.Operation)
				{
				case EOperation::SessionBegin:
				{
					if (Targets.Num() < 2) return FailClosed(Out, Error, TEXT("multi_target_binding_required"), TEXT("A multi-target sculpt session requires at least two resolved targets."));
					if (!SetActorSelection(Targets, Out, Error)) return FailClosed(Out, Error, TEXT("multi_target_selection_failed"), Error);
					FString SessionId;
					Arguments->TryGetStringField(TEXT("session_id"), SessionId);
					if (SessionId.IsEmpty()) SessionId = FString::Printf(TEXT("mts_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
					if (!IsSafeIdentifier(SessionId)) return FailClosed(Out, Error, TEXT("session_id_invalid"), TEXT("session_id contains unsupported characters."));
					bool bSessionExists = IFileManager::Get().FileExists(*SessionManifestPath(SessionId));
					{
						FScopeLock Lock(&SessionLock);
						bSessionExists = bSessionExists || Sessions.Contains(SessionId);
					}
					if (bSessionExists) return FailClosed(Out, Error, TEXT("session_id_exists"), TEXT("Refusing to overwrite an existing sculpt session manifest."));
					FSculptSession Session;
					Session.Id = SessionId;
					Session.CreatedAt = FDateTime::UtcNow().ToIso8601();
					Session.UpdatedAt = Session.CreatedAt;
					Session.LastOperation = Spec.Name;
					for (UObject* Target : Targets) Session.TargetPaths.Add(Target->GetPathName());
					FString SessionError;
					if (!PersistSession(Session, SessionError)) return FailClosed(Out, Error, TEXT("session_manifest_write_failed"), SessionError);
					{
						FScopeLock Lock(&SessionLock);
						Sessions.Add(SessionId, Session);
					}
					Receipt->SetStringField(TEXT("session_id"), SessionId);
					Receipt->SetStringField(TEXT("session_manifest_path"), SessionManifestPath(SessionId));
					Receipt->SetArrayField(TEXT("session_targets"), ToJson(Session.TargetPaths));
					Out->SetStringField(TEXT("session_id"), SessionId);
					Out->SetStringField(TEXT("session_manifest_path"), SessionManifestPath(SessionId));
					Out->SetArrayField(TEXT("session_targets"), ToJson(Session.TargetPaths));
					Summary = FString::Printf(TEXT("Created persistent native multi-target sculpt session %s for %d targets."), *SessionId, Targets.Num());
					return true;
				}
				case EOperation::Selection:
					if (Targets.IsEmpty()) return FailClosed(Out, Error, TEXT("target_binding_required"), TEXT("No Mesh Terrain actors resolved for selection."));
					if (!SetActorSelection(Targets, Out, Error)) return FailClosed(Out, Error, TEXT("selection_readback_failed"), Error);
					Summary = FString::Printf(TEXT("Selected %d Mesh Terrain actor(s) with exact readback."), Targets.Num());
					return true;
				case EOperation::LayerMerge:
				case EOperation::LayerDuplicate:
				case EOperation::LayerReorder:
				{
					if (Targets.IsEmpty()) return FailClosed(Out, Error, TEXT("target_binding_required"), TEXT("A bound Mesh Terrain target is required for sculpt-layer mutation."));
					FJsonObjectRef Child = ChildArguments(Arguments);
					const TCHAR* Op = Spec.Operation == EOperation::LayerMerge ? TEXT("merge") : Spec.Operation == EOperation::LayerDuplicate ? TEXT("duplicate") : TEXT("reorder");
					Child->SetStringField(TEXT("operation"), Op);
					Child->SetStringField(TEXT("target_asset"), Targets[0]->GetPathName());
					return ExecuteChild(Registry, {TEXT("mesh_terrain_sculpt_layer_mutate")}, Child, true, Targets[0]->GetPathName(), Out, Summary, Error);
				}
				case EOperation::Stamp:
				{
					if (Targets.IsEmpty()) return FailClosed(Out, Error, TEXT("target_binding_required"), TEXT("A bound Mesh Terrain target is required for stamping."));
					FString ReadyError;
					if (!EnsureMeshPartitionReady(Targets[0], ReadyError)) return FailClosed(Out, Error, TEXT("mesh_partition_initialization_failed"), ReadyError);
					FJsonObjectRef Child = ChildArguments(Arguments);
					Child->SetStringField(TEXT("target_asset"), Targets[0]->GetPathName());
					return ExecuteChild(Registry, {TEXT("mesh_partition_modifier_create_texture_patch"), TEXT("mesh_partition_modifier_create_patch")}, Child, true, Targets[0]->GetPathName(), Out, Summary, Error);
				}
				case EOperation::Erosion:
				{
					if (Targets.IsEmpty()) return FailClosed(Out, Error, TEXT("target_binding_required"), TEXT("A bound Mesh Terrain target is required for erosion."));
					FString ReadyError;
					if (!EnsureMeshPartitionReady(Targets[0], ReadyError)) return FailClosed(Out, Error, TEXT("mesh_partition_initialization_failed"), ReadyError);
					FJsonObjectRef Child = ChildArguments(Arguments);
					Child->SetStringField(TEXT("target_asset"), Targets[0]->GetPathName());
					return ExecuteChild(Registry, {TEXT("mesh_terrain_slope_erode_apply")}, Child, true, Targets[0]->GetPathName(), Out, Summary, Error);
				}
				case EOperation::HoleCaveOverhang:
				{
					if (Targets.IsEmpty()) return FailClosed(Out, Error, TEXT("target_binding_required"), TEXT("A bound Mesh Terrain target is required for hole/cave/overhang mutation."));
					{
						FString ReadyError;
						if (!EnsureMeshPartitionReady(Targets[0], ReadyError)) return FailClosed(Out, Error, TEXT("mesh_partition_initialization_failed"), ReadyError);
					}
					FString HoleType;
					Arguments->TryGetStringField(TEXT("hole_type"), HoleType);
					if (HoleType.IsEmpty()) return FailClosed(Out, Error, TEXT("hole_type_required"), TEXT("hole_type is required."));
					FJsonObjectRef Child = ChildArguments(Arguments);
					Child->SetStringField(TEXT("target_asset"), Targets[0]->GetPathName());
					Child->SetStringField(TEXT("operation"), HoleType);
					return ExecuteChild(Registry, {TEXT("mesh_partition_modifier_create_boolean")}, Child, true, Targets[0]->GetPathName(), Out, Summary, Error);
				}
				case EOperation::SeamRepair:
				{
					if (Targets.Num() < 2) return FailClosed(Out, Error, TEXT("seam_targets_required"), TEXT("Seam repair requires at least two resolved Mesh Terrain targets."));
					if (!SetActorSelection(Targets, Out, Error)) return FailClosed(Out, Error, TEXT("seam_selection_failed"), Error);
					FJsonObjectRef Child = ChildArguments(Arguments);
					Child->SetStringField(TEXT("target_asset"), Targets[0]->GetPathName());
					return ExecuteChild(Registry, {TEXT("mesh_partition_stitch")}, Child, true, Targets[0]->GetPathName(), Out, Summary, Error);
				}
				case EOperation::GpuStats:
				{
					FJsonObjectRef Child = ChildArguments(Arguments);
					if (!Targets.IsEmpty()) Child->SetStringField(TEXT("target_asset"), Targets[0]->GetPathName());
					return ExecuteChild(Registry, {TEXT("mesh_partition_build_perf_stats_get")}, Child, false, FString(), Out, Summary, Error);
				}
				case EOperation::WorldPartitionCommit:
				{
					if (Targets.IsEmpty()) return FailClosed(Out, Error, TEXT("target_binding_required"), TEXT("World Partition commit requires at least one persistent Mesh Terrain target."));
					if (!SaveTargets(Context, Targets, Receipt, Error)) return FailClosed(Out, Error, TEXT("pre_commit_save_failed"), Error);
					FJsonObjectRef Child = ChildArguments(Arguments);
					Child->SetStringField(TEXT("target_asset"), Targets[0]->GetPathName());
					if (!ExecuteChild(Registry, {TEXT("mesh_partition_build_submit")}, Child, true, Targets[0]->GetPathName(), Out, Summary, Error)) return false;
					FJsonObjectRef ReadbackArgs = ChildArguments(Arguments);
					ReadbackArgs->SetStringField(TEXT("target_asset"), Targets[0]->GetPathName());
					FJsonObjectRef ReadbackOut = MakeShared<FJsonObject>();
					FString ReadbackSummary;
					FString ReadbackError;
					if (!ExecuteChild(Registry, {TEXT("mesh_partition_runtime_cell_readback")}, ReadbackArgs, false, FString(), ReadbackOut, ReadbackSummary, ReadbackError))
						return FailClosed(Out, Error, TEXT("world_partition_readback_failed"), ReadbackError);
					const TSharedPtr<FJsonObject>* NativeReadback = nullptr;
					if (!ReadbackOut->TryGetObjectField(TEXT("native_child_result"), NativeReadback) || !NativeReadback || !NativeReadback->IsValid()
						|| !ValidateReadbackResult((*NativeReadback).ToSharedRef(), Targets[0]->GetPathName()))
						return FailClosed(Out, Error, TEXT("world_partition_readback_invalid"), TEXT("World Partition child did not return matching resolved target readback."));
					Out->SetObjectField(TEXT("world_partition_readback"), ReadbackOut);
					Out->SetBoolField(TEXT("world_partition_readback_verified"), true);
					return true;
				}
				case EOperation::LandscapeMeshDiff:
				{
					FString LandscapeId;
					FString MeshId;
					Arguments->TryGetStringField(TEXT("landscape_actor"), LandscapeId);
					Arguments->TryGetStringField(TEXT("mesh_asset"), MeshId);
					UObject* Landscape = ResolveOne(Context, LandscapeId);
					UObject* Mesh = ResolveOne(Context, MeshId);
					if (!Landscape || !Mesh) return FailClosed(Out, Error, TEXT("diff_targets_required"), TEXT("Both landscape_actor and mesh_asset must resolve."));
					const FJsonObjectRef LandscapeSnapshot = Snapshot(Landscape);
					const FJsonObjectRef MeshSnapshot = Snapshot(Mesh);
					Out->SetObjectField(TEXT("landscape_snapshot"), LandscapeSnapshot);
					Out->SetObjectField(TEXT("mesh_snapshot"), MeshSnapshot);
					Out->SetBoolField(TEXT("property_fingerprints_equal"), SnapshotString(LandscapeSnapshot, TEXT("property_crc32")) == SnapshotString(MeshSnapshot, TEXT("property_crc32")));
					Out->SetBoolField(TEXT("class_equal"), Landscape->GetClass() == Mesh->GetClass());
					Summary = TEXT("Compared Landscape and Mesh Terrain targets with native snapshots and fingerprints.");
					return true;
				}
				case EOperation::MaterialAttributeReadback:
				{
					if (Targets.IsEmpty()) return FailClosed(Out, Error, TEXT("target_binding_required"), TEXT("A target is required for material/attribute readback."));
					TArray<TSharedPtr<FJsonValue>> Rows;
					for (UObject* Target : Targets)
					{
						FJsonObjectRef Row = Snapshot(Target);
						TArray<TSharedPtr<FJsonValue>> Materials;
						if (UMeshComponent* Component = Cast<UMeshComponent>(Target))
						{
							for (int32 Index = 0; Index < Component->GetNumMaterials(); ++Index) Materials.Add(MakeShared<FJsonValueObject>(FSololmcpEditorServices::MakeObjectReference(Component->GetMaterial(Index))));
						}
						else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Target))
						{
							for (const FStaticMaterial& Material : StaticMesh->GetStaticMaterials()) Materials.Add(MakeShared<FJsonValueObject>(FSololmcpEditorServices::MakeObjectReference(Material.MaterialInterface)));
						}
						Row->SetArrayField(TEXT("materials"), Materials);
						bool bIncludeDependencies = true;
						Arguments->TryGetBoolField(TEXT("include_dependencies"), bIncludeDependencies);
						if (bIncludeDependencies && FPackageName::IsValidObjectPath(Target->GetPathName()))
						{
							FString DependencyError;
							Row->SetArrayField(TEXT("dependencies"), ToJson(Context.Services.GetAssetDependencies(Target->GetPathName(), DependencyError)));
							Row->SetStringField(TEXT("dependency_error"), DependencyError);
						}
						Rows.Add(MakeShared<FJsonValueObject>(Row));
					}
					Out->SetArrayField(TEXT("target_readback"), Rows);
					Summary = FString::Printf(TEXT("Read material, attribute, package, and dependency data from %d target(s)."), Rows.Num());
					return true;
				}
				case EOperation::NavCollisionHlodReceipt:
				{
					const TArray<TArray<FString>> Checks = {{TEXT("mesh_terrain_collision_navigation_audit")}, {TEXT("mesh_terrain_world_partition_streaming_audit")}, {TEXT("hlod_list")}};
					TArray<TSharedPtr<FJsonValue>> CheckRows;
					for (const TArray<FString>& Candidates : Checks)
					{
						FJsonObjectRef CheckOut = MakeShared<FJsonObject>();
						FString CheckSummary;
						FString CheckError;
						FJsonObjectRef CheckArgs = ChildArguments(Arguments);
						if (!Targets.IsEmpty()) CheckArgs->SetStringField(TEXT("target_asset"), Targets[0]->GetPathName());
						if (!ExecuteChild(Registry, Candidates, CheckArgs, false, FString(), CheckOut, CheckSummary, CheckError))
						{
							Out->SetArrayField(TEXT("checks"), CheckRows);
							return FailClosed(Out, Error, TEXT("delivery_receipt_check_failed"), CheckError);
						}
						CheckRows.Add(MakeShared<FJsonValueObject>(CheckOut));
					}
					Out->SetArrayField(TEXT("checks"), CheckRows);
					Out->SetBoolField(TEXT("navigation_collision_hlod_verified"), true);
					Summary = TEXT("Navigation, collision, World Partition, and HLOD checks returned native evidence.");
					return true;
				}
				case EOperation::CheckpointCreate:
					return ExecuteCheckpointCreate(Context, Arguments, Targets, Out, Receipt, Summary, Error);
				case EOperation::CheckpointRestore:
					return ExecuteCheckpointRestore(Context, Arguments, Out, Receipt, Summary, Error);
				}
				return FailClosed(Out, Error, TEXT("unsupported_operation"), TEXT("Mesh Terrain completion operation is not implemented."));
				}();
			}
		}

		if (bEntryOk && bSessionBindingOk && Spec.Operation != EOperation::SessionBegin && !BoundSessionId.IsEmpty())
		{
			FString SessionError;
			if (!ConsumeSession(BoundSession, Spec.Name, Out, SessionError))
			{
				Out->SetStringField(TEXT("session_manifest_error"), SessionError);
				if (bOk) bOk = FailClosed(Out, Error, TEXT("session_manifest_update_failed"), SessionError);
			}
		}

		FString ExitPrevious;
		FString ExitCurrent;
		FString ExitError;
		const bool bExitOk = TerrainModeGuard::ForceSelectionMode(ExitPrevious, ExitCurrent, ExitError);
		Receipt->SetBoolField(TEXT("select_mode_exit_ok"), bExitOk);
		Receipt->SetStringField(TEXT("select_mode_before_exit"), ExitPrevious);
		Receipt->SetStringField(TEXT("select_mode_after_operation"), ExitCurrent);
		Receipt->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		if (!bExitOk)
		{
			CompleteReceipt(Receipt, TEXT("failed_select_mode_exit"), Targets);
			return FailClosed(Out, Error, TEXT("select_mode_exit_failed"), ExitError);
		}
		Out->SetBoolField(TEXT("select_mode_restored"), true);
		if (!bOk)
		{
			CompleteReceipt(Receipt, TEXT("failed_closed"), Targets);
			if (Error.IsEmpty()) Error = TEXT("Mesh Terrain completion operation failed closed.");
			return false;
		}
		CompleteReceipt(Receipt, Spec.bMutation ? TEXT("completed_write_readback_verified") : TEXT("completed_readback_verified"), Targets);
		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		return true;
#endif
	}

	static void RegisterOne(FSololmcpToolRegistry& Registry, const FSpec& Spec)
	{
		FSololmcpToolDefinition Definition;
		Definition.Name = Spec.Name;
		Definition.Description = Spec.Description;
		Definition.InputSchema = Schema();
		CloseObjectSchemas(Definition.InputSchema);
		Definition.CacheTtlSeconds = 0;
		Definition.bUsesExternalPython = false;
		Definition.Execute = [&Registry, Spec](const FSololmcpToolExecutionContext& Context, const FJsonObjectRef& Arguments,
			FJsonObjectRef& Out, FString& Summary, FString& Error)
		{
			return Execute(Registry, Spec, Context, Arguments, Out, Summary, Error);
		};
		Registry.Register(Definition);
	}
}

void RegisterMeshTerrainCompletionTools(FSololmcpToolRegistry& Registry)
{
	for (const MeshTerrainCompletion::FSpec& Spec : MeshTerrainCompletion::Specs)
	{
		MeshTerrainCompletion::RegisterOne(Registry, Spec);
	}
}
}
