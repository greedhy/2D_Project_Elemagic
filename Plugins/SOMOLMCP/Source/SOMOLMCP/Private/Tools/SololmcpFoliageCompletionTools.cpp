#include "Tools/SololmcpFoliageCompletionTools.h"

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "HAL/FileManager.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/SavePackage.h"

namespace UE::SOMOLMCP
{
namespace
{
	using FHandler = TFunction<bool(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>&,
		TSharedRef<FJsonObject>&,
		FString&,
		FString&)>;

	static TSharedRef<FJsonObject> VectorSchema(const FString& Description = FString())
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("x"), FSololmcpSchemaBuilder::Number()},
			{TEXT("y"), FSololmcpSchemaBuilder::Number()},
			{TEXT("z"), FSololmcpSchemaBuilder::Number()}
		}, {}, Description, false);
	}

	static TSharedRef<FJsonObject> FoliageAuthorSchema(const TArray<FString>& Required = {})
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("foliage_type_path"), FSololmcpSchemaBuilder::String(TEXT("Target FoliageType asset path."))},
			{TEXT("source_foliage_type_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("target_foliage_type_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("mesh_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor name, label, or object path."))},
			{TEXT("component"), FSololmcpSchemaBuilder::String(TEXT("Component object name or path."))},
			{TEXT("center"), VectorSchema()},
			{TEXT("location"), VectorSchema()},
			{TEXT("min"), VectorSchema()},
			{TEXT("max"), VectorSchema()},
			{TEXT("rotation"), VectorSchema(TEXT("Pitch, yaw, roll in x, y, z."))},
			{TEXT("scale"), VectorSchema()},
			{TEXT("normal"), VectorSchema()},
			{TEXT("points"), FSololmcpSchemaBuilder::Array(VectorSchema())},
			{TEXT("foliage_types"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
				{TEXT("path"), FSololmcpSchemaBuilder::String()},
				{TEXT("weight"), FSololmcpSchemaBuilder::Number()}
			}, {TEXT("path")}, FString(), false))},
			{TEXT("instance_index"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("count"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("target_count"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("seed"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("radius_cm"), FSololmcpSchemaBuilder::Number()},
			{TEXT("width_cm"), FSololmcpSchemaBuilder::Number()},
			{TEXT("spacing_cm"), FSololmcpSchemaBuilder::Number()},
			{TEXT("collision_radius_cm"), FSololmcpSchemaBuilder::Number()},
			{TEXT("trace_half_height_cm"), FSololmcpSchemaBuilder::Number()},
			{TEXT("strength"), FSololmcpSchemaBuilder::Number()},
			{TEXT("density"), FSololmcpSchemaBuilder::Number()},
			{TEXT("density_factor"), FSololmcpSchemaBuilder::Number()},
			{TEXT("scale_min"), FSololmcpSchemaBuilder::Number()},
			{TEXT("scale_max"), FSololmcpSchemaBuilder::Number()},
			{TEXT("max_slope_degrees"), FSololmcpSchemaBuilder::Number()},
			{TEXT("align_max_angle"), FSololmcpSchemaBuilder::Number()},
			{TEXT("project_to_surface"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("align_to_normal"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("random_yaw"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("collision_avoidance"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("remove_source"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("destroy_component"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("save"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("properties"), FSololmcpSchemaBuilder::Object({
				{TEXT("density"), FSololmcpSchemaBuilder::Number()},
				{TEXT("density_adjustment_factor"), FSololmcpSchemaBuilder::Number()},
				{TEXT("radius"), FSololmcpSchemaBuilder::Number()},
				{TEXT("single_instance_override_radius"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("single_instance_radius"), FSololmcpSchemaBuilder::Number()},
				{TEXT("scale_min"), VectorSchema()},
				{TEXT("scale_max"), VectorSchema()},
				{TEXT("z_offset_min"), FSololmcpSchemaBuilder::Number()},
				{TEXT("z_offset_max"), FSololmcpSchemaBuilder::Number()},
				{TEXT("align_to_normal"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("align_max_angle"), FSololmcpSchemaBuilder::Number()},
				{TEXT("random_yaw"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("random_pitch_angle"), FSololmcpSchemaBuilder::Number()},
				{TEXT("slope_min"), FSololmcpSchemaBuilder::Number()},
				{TEXT("slope_max"), FSololmcpSchemaBuilder::Number()},
				{TEXT("height_min"), FSololmcpSchemaBuilder::Number()},
				{TEXT("height_max"), FSololmcpSchemaBuilder::Number()},
				{TEXT("collision_with_world"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("collision_scale"), VectorSchema()},
				{TEXT("minimum_layer_weight"), FSololmcpSchemaBuilder::Number()},
				{TEXT("cull_start"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("cull_end"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("mobility"), FSololmcpSchemaBuilder::String(TEXT("static, stationary, or movable"), {TEXT("static"), TEXT("stationary"), TEXT("movable")})},
				{TEXT("cast_shadow"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("cast_dynamic_shadow"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("cast_static_shadow"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("cast_contact_shadow"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("affect_dynamic_indirect_lighting"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("affect_distance_field_lighting"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("visible_in_ray_tracing"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("visible_in_reflections"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("evaluate_world_position_offset"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("world_position_offset_disable_distance"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("nanite_pixel_programmable_distance"), FSololmcpSchemaBuilder::Number()},
				{TEXT("render_custom_depth"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("custom_depth_stencil_value"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("enable_density_scaling"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("enable_cull_distance_scaling"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("virtual_texture_cull_mips"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("include_in_hlod"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_density"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_radius"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_align_to_normal"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_random_yaw"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_scaling"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_scale_x"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_scale_y"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_scale_z"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_random_pitch"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_ground_slope"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_height"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_landscape_layers"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_z_offset"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("reapply_collision"), FSololmcpSchemaBuilder::Boolean()}
			}, {}, FString(), false)}
		}, Required, TEXT("Native foliage authoring request. Unknown or unsupported operations fail closed."), false);
	}

	static bool TryVector(const TSharedRef<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
	{
		const TSharedPtr<FJsonObject>* Child = nullptr;
		if (!Object->TryGetObjectField(Field, Child) || !Child || !Child->IsValid())
		{
			return false;
		}
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!(*Child)->TryGetNumberField(TEXT("x"), X)
			|| !(*Child)->TryGetNumberField(TEXT("y"), Y)
			|| !(*Child)->TryGetNumberField(TEXT("z"), Z))
		{
			return false;
		}
		Out = FVector(X, Y, Z);
		return FMath::IsFinite(Out.X) && FMath::IsFinite(Out.Y) && FMath::IsFinite(Out.Z);
	}

	static bool TryPoints(const TSharedRef<FJsonObject>& Object, const TCHAR* Field, TArray<FVector>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(Field, Values) || !Values)
		{
			return false;
		}
		Out.Reset(Values->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Point = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Point.IsValid())
			{
				return false;
			}
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (!Point->TryGetNumberField(TEXT("x"), X)
				|| !Point->TryGetNumberField(TEXT("y"), Y)
				|| !Point->TryGetNumberField(TEXT("z"), Z))
			{
				return false;
			}
			const FVector Parsed(X, Y, Z);
			if (!FMath::IsFinite(Parsed.X) || !FMath::IsFinite(Parsed.Y) || !FMath::IsFinite(Parsed.Z))
			{
				return false;
			}
			Out.Add(Parsed);
		}
		return true;
	}

	static double Number(const TSharedRef<FJsonObject>& Object, const TCHAR* Field, double DefaultValue)
	{
		double Value = DefaultValue;
		Object->TryGetNumberField(Field, Value);
		return Value;
	}

	static int32 Integer(const TSharedRef<FJsonObject>& Object, const TCHAR* Field, int32 DefaultValue)
	{
		double Value = static_cast<double>(DefaultValue);
		Object->TryGetNumberField(Field, Value);
		return FMath::RoundToInt(Value);
	}

	static bool Boolean(const TSharedRef<FJsonObject>& Object, const TCHAR* Field, bool DefaultValue)
	{
		bool Value = DefaultValue;
		Object->TryGetBoolField(Field, Value);
		return Value;
	}

	static void Fail(
		TSharedRef<FJsonObject>& Out,
		FString& Error,
		const FString& Code,
		const FString& Message)
	{
		Error = Message;
		Out->SetBoolField(TEXT("success"), false);
		Out->SetStringField(TEXT("status"), TEXT("failed"));
		Out->SetStringField(TEXT("error_code"), Code);
		Out->SetStringField(TEXT("error"), Message);
		Out->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
		Out->SetBoolField(TEXT("python_used"), false);
	}

	static TSharedRef<FJsonObject> VectorJson(const FVector& Value)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("x"), Value.X);
		Out->SetNumberField(TEXT("y"), Value.Y);
		Out->SetNumberField(TEXT("z"), Value.Z);
		return Out;
	}

	struct FFoliageTarget
	{
		UWorld* World = nullptr;
		AInstancedFoliageActor* Actor = nullptr;
		UFoliageType* Type = nullptr;
		FFoliageInfo* Info = nullptr;
		FString TypePath;
	};

	class FAtomicFoliageTransaction
	{
	public:
		explicit FAtomicFoliageTransaction(const FText& Description)
			: Previous(Active())
		{
			if (GEditor && GEditor->Trans)
			{
				QueueLengthBefore = GEditor->Trans->GetQueueLength();
				bOpen = GEditor->BeginTransaction(Description) != INDEX_NONE;
				if (bOpen)
				{
					Active() = this;
				}
			}
		}

		~FAtomicFoliageTransaction()
		{
			Finish(false);
			if (Active() == this)
			{
				Active() = Previous;
			}
		}

		void Commit()
		{
			Finish(true);
		}

		bool Cancel()
		{
			return Finish(false);
		}

		bool IsOpen() const
		{
			return bOpen;
		}

		static void CommitActive()
		{
			if (FAtomicFoliageTransaction* Transaction = Active())
			{
				Transaction->Commit();
			}
		}

	private:
		static FAtomicFoliageTransaction*& Active()
		{
			static FAtomicFoliageTransaction* Current = nullptr;
			return Current;
		}

		bool Finish(bool bCommit)
		{
			if (!bOpen || !GEditor)
			{
				return true;
			}
			GEditor->EndTransaction();
			bOpen = false;
			if (!bCommit && GEditor->Trans && !GEditor->Trans->IsActive()
				&& GEditor->Trans->GetQueueLength() > QueueLengthBefore)
			{
				return GEditor->UndoTransaction();
			}
			return true;
		}

		FAtomicFoliageTransaction* Previous = nullptr;
		int32 QueueLengthBefore = 0;
		bool bOpen = false;
	};

	static bool ResolveTarget(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		const TCHAR* PathField,
		bool bCreateInfo,
		FFoliageTarget& OutTarget,
		TSharedRef<FJsonObject>& Out,
		FString& Error)
	{
		if (!Arguments->TryGetStringField(PathField, OutTarget.TypePath) || OutTarget.TypePath.IsEmpty())
		{
			Fail(Out, Error, TEXT("missing_foliage_type"), FString::Printf(TEXT("%s is required."), PathField));
			return false;
		}
		OutTarget.Type = Cast<UFoliageType>(Context.Services.LoadAsset(OutTarget.TypePath, Error));
		if (!OutTarget.Type)
		{
			Fail(Out, Error, TEXT("invalid_foliage_type"), Error.IsEmpty() ? TEXT("The asset is not a UFoliageType.") : Error);
			return false;
		}
		OutTarget.World = Context.Services.GetEditorWorld(Error);
		if (!OutTarget.World || !OutTarget.World->GetCurrentLevel())
		{
			Fail(Out, Error, TEXT("no_editor_world"), Error.IsEmpty() ? TEXT("No writable editor world/current level is available.") : Error);
			return false;
		}
		// Load the Foliage module explicitly before touching InstancedFoliageActor:
		// its class registration is required, and concurrent lazy module loads used
		// to race under parallel job dispatch (the dispatch layer now serializes
		// every foliage_* tool, so this LoadModule is single-threaded again).
		FModuleManager::Get().LoadModule(TEXT("Foliage"));
		OutTarget.Actor = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(OutTarget.World, false);
		if (!OutTarget.Actor && bCreateInfo)
		{
			OutTarget.World->GetCurrentLevel()->Modify();
			OutTarget.Actor = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(OutTarget.World, true);
		}
		if (!OutTarget.Actor)
		{
			Fail(Out, Error, TEXT("no_foliage_actor"), TEXT("Unable to resolve or create the current level's InstancedFoliageActor."));
			return false;
		}
		// Headless IFA spawns have no root component. AddInstanceBaseId ->
		// IFoliageEditModuleBase::ShouldIgnoreComponentForBaseID dereferences
		// BaseComponent with no null guard (FoliageEditModule.cpp), so any
		// AddInstance with a null base crashes with 0xC0000005. Guarantee a
		// valid root here and set BaseComponent on every instance we build.
		if (!OutTarget.Actor->GetRootComponent())
		{
			USceneComponent* Root = NewObject<USceneComponent>(OutTarget.Actor);
			OutTarget.Actor->SetRootComponent(Root);
			Root->RegisterComponent();
		}
		OutTarget.Info = OutTarget.Actor->FindInfo(OutTarget.Type);
		if (!OutTarget.Info && bCreateInfo)
		{
			OutTarget.Actor->Modify();
			OutTarget.Info = OutTarget.Actor->FindOrAddMesh(OutTarget.Type);
		}
		if (!OutTarget.Info && bCreateInfo)
		{
			Fail(Out, Error, TEXT("no_foliage_info"), TEXT("Unable to resolve or create FFoliageInfo for the requested type."));
			return false;
		}
		return true;
	}

	struct FPackageSaveTarget
	{
		UPackage* Package = nullptr;
		UObject* TopLevelObject = nullptr;
		FString Filename;
		bool bExisted = false;
		TArray<uint8> OriginalBytes;
		FString HashBefore;
	};

	static FString PackageFileHash(const FString& Filename)
	{
		if (!IFileManager::Get().FileExists(*Filename))
		{
			return FString();
		}
		const FMD5Hash Hash = FMD5Hash::HashFile(*Filename);
		return Hash.IsValid() ? BytesToHex(Hash.GetBytes(), Hash.GetSize()) : FString();
	}

	static bool HasValidPackageSummary(const FString& Filename)
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
		if (!Reader)
		{
			return false;
		}
		FPackageFileSummary Summary;
		*Reader << Summary;
		return !Reader->IsError() && Summary.Tag == PACKAGE_FILE_TAG;
	}

	static bool SaveRelatedPackages(
		const TArray<UObject*>& Objects,
		bool bSave,
		TSharedRef<FJsonObject>& Out,
		FString& Error)
	{
		TArray<FPackageSaveTarget> Targets;
		TSet<UPackage*> SeenPackages;
		for (UObject* Object : Objects)
		{
			if (!Object)
			{
				continue;
			}
			UPackage* Package = Object->GetPackage();
			if (!Package || Package == GetTransientPackage() || SeenPackages.Contains(Package))
			{
				continue;
			}
			const FString PackageName = Package->GetName();
			if (!FPackageName::IsValidLongPackageName(PackageName))
			{
				continue;
			}
			SeenPackages.Add(Package);
			FPackageSaveTarget& Target = Targets.AddDefaulted_GetRef();
			Target.Package = Package;
			Target.TopLevelObject = Package->ContainsMap() ? Object->GetWorld() : Object;
			Target.Filename = FPackageName::LongPackageNameToFilename(
				PackageName,
				Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
		}

		if (Targets.IsEmpty())
		{
			Fail(Out, Error, TEXT("no_persistent_package"), TEXT("No persistent package was resolved for the mutated objects."));
			return false;
		}

		for (FPackageSaveTarget& Target : Targets)
		{
			Target.Package->MarkPackageDirty();
		}
		if (!bSave)
		{
			Out->SetBoolField(TEXT("saved"), false);
			Out->SetBoolField(TEXT("persistence_verified"), false);
			Out->SetStringField(TEXT("save_status"), TEXT("dirty_only_unverified"));
			return true;
		}

		for (FPackageSaveTarget& Target : Targets)
		{
			Target.bExisted = IFileManager::Get().FileExists(*Target.Filename);
			Target.HashBefore = PackageFileHash(Target.Filename);
			if (Target.bExisted && !FFileHelper::LoadFileToArray(Target.OriginalBytes, *Target.Filename))
			{
				Fail(Out, Error, TEXT("package_backup_failed"), FString::Printf(TEXT("Unable to checkpoint package file before save: %s"), *Target.Filename));
				return false;
			}
		}

		auto RestorePackageFiles = [&Targets]()
		{
			bool bRestored = true;
			for (const FPackageSaveTarget& Target : Targets)
			{
				if (Target.bExisted)
				{
					bRestored &= FFileHelper::SaveArrayToFile(Target.OriginalBytes, *Target.Filename)
						&& PackageFileHash(Target.Filename) == Target.HashBefore;
				}
				else
				{
					bRestored &= IFileManager::Get().Delete(*Target.Filename, false, true, true)
						|| !IFileManager::Get().FileExists(*Target.Filename);
				}
			}
			return bRestored;
		};

		TArray<TSharedPtr<FJsonValue>> PackageRows;
		bool bAllVerified = true;
		bool bAnyDiskPayloadChanged = false;
		for (FPackageSaveTarget& Target : Targets)
		{
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			const bool bSaved = UPackage::SavePackage(Target.Package, Target.TopLevelObject, *Target.Filename, SaveArgs);
			const int64 FileSize = IFileManager::Get().FileSize(*Target.Filename);
			const FString HashAfter = PackageFileHash(Target.Filename);
			const bool bDiskPayloadChanged = !Target.bExisted || Target.HashBefore != HashAfter;
			const bool bVerified = bSaved
				&& FileSize > 0
				&& !HashAfter.IsEmpty()
				&& HasValidPackageSummary(Target.Filename)
				&& FPackageName::DoesPackageExist(Target.Package->GetName())
				&& !Target.Package->IsDirty();
			bAllVerified &= bVerified;
			bAnyDiskPayloadChanged |= bDiskPayloadChanged;

			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("package"), Target.Package->GetName());
			Row->SetStringField(TEXT("file"), Target.Filename);
			Row->SetBoolField(TEXT("external_actor_package"), Target.Package->GetName().Contains(TEXT("/ExternalActors/")));
			Row->SetBoolField(TEXT("saved"), bSaved);
			Row->SetBoolField(TEXT("disk_verified"), bVerified);
			Row->SetBoolField(TEXT("disk_payload_changed"), bDiskPayloadChanged);
			Row->SetBoolField(TEXT("package_summary_verified"), HasValidPackageSummary(Target.Filename));
			Row->SetNumberField(TEXT("file_size"), static_cast<double>(FMath::Max<int64>(0, FileSize)));
			Row->SetStringField(TEXT("file_hash_before"), Target.HashBefore);
			Row->SetStringField(TEXT("file_hash_after"), HashAfter);
			PackageRows.Add(MakeShared<FJsonValueObject>(Row));
			if (!bVerified)
			{
				break;
			}
		}

		Out->SetArrayField(TEXT("saved_packages"), PackageRows);
		if (!bAllVerified || !bAnyDiskPayloadChanged || PackageRows.Num() != Targets.Num())
		{
			const bool bDiskRollbackVerified = RestorePackageFiles();
			Out->SetBoolField(TEXT("disk_rollback_verified"), bDiskRollbackVerified);
			Fail(Out, Error,
				bDiskRollbackVerified ? TEXT("save_or_disk_verify_failed") : TEXT("disk_rollback_failed"),
				bDiskRollbackVerified
					? TEXT("A related map/ExternalActor package failed to save, change its disk payload, or pass disk verification; all prior package files were restored and hash-verified.")
					: TEXT("A related package failed and at least one prior package file could not be restored to its original hash."));
			return false;
		}

		Out->SetBoolField(TEXT("saved"), true);
		Out->SetBoolField(TEXT("persistence_verified"), true);
		Out->SetStringField(TEXT("save_status"), TEXT("saved_packages_disk_verified"));
		Out->SetStringField(TEXT("readback_verification_mode"), TEXT("post_mutation_memory_state_plus_package_summary_and_disk_hash"));
		return true;
	}

	static bool SaveWorld(
		const FSololmcpToolExecutionContext&,
		UWorld* World,
		const TArray<UObject*>& MutationTargets,
		bool bSave,
		TSharedRef<FJsonObject>& Out,
		FString& Error)
	{
		if (!World)
		{
			Fail(Out, Error, TEXT("no_world"), TEXT("No world was available for persistence."));
			return false;
		}
		if (!SaveRelatedPackages(MutationTargets, bSave, Out, Error))
		{
			return false;
		}
		Out->SetStringField(TEXT("world_context_package"), World->GetPackage()->GetName());
		return true;
	}

	static bool SaveType(
		const FSololmcpToolExecutionContext&,
		UFoliageType* Type,
		bool bSave,
		TSharedRef<FJsonObject>& Out,
		FString& Error)
	{
		if (!Type)
		{
			Fail(Out, Error, TEXT("no_foliage_type"), TEXT("No FoliageType was available for persistence."));
			return false;
		}
		if (!SaveRelatedPackages({Type}, bSave, Out, Error))
		{
			return false;
		}
		Out->SetStringField(TEXT("saved_asset"), Type->GetPathName());
		return true;
	}

	static TSharedRef<FJsonObject> TransformJson(const FTransform& Transform)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("location"), VectorJson(Transform.GetLocation()));
		Out->SetObjectField(TEXT("rotation"), VectorJson(FVector(Transform.Rotator().Pitch, Transform.Rotator().Yaw, Transform.Rotator().Roll)));
		Out->SetObjectField(TEXT("scale"), VectorJson(Transform.GetScale3D()));
		return Out;
	}

	static TSharedRef<FJsonObject> FoliageStateReadback(const FFoliageInfo* Info)
	{
		TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
		FString Canonical;
		TArray<TSharedPtr<FJsonValue>> Samples;
		const int32 Count = Info ? Info->Instances.Num() : 0;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FTransform Transform = Info->Instances[Index].GetInstanceWorldTransform();
			const FVector Location = Transform.GetLocation();
			const FRotator Rotation = Transform.Rotator();
			const FVector Scale = Transform.GetScale3D();
			Canonical += FString::Printf(TEXT("%d:%.9g,%.9g,%.9g|%.9g,%.9g,%.9g|%.9g,%.9g,%.9g;"),
				Index, Location.X, Location.Y, Location.Z, Rotation.Pitch, Rotation.Yaw, Rotation.Roll, Scale.X, Scale.Y, Scale.Z);
			if (Samples.Num() < 16)
			{
				TSharedRef<FJsonObject> Sample = TransformJson(Transform);
				Sample->SetNumberField(TEXT("instance_index"), Index);
				Samples.Add(MakeShared<FJsonValueObject>(Sample));
			}
		}
		State->SetNumberField(TEXT("instance_count"), Count);
		State->SetStringField(TEXT("transform_crc32"), FString::Printf(TEXT("%08X"), FCrc::StrCrc32(*Canonical)));
		State->SetArrayField(TEXT("transform_samples"), Samples);
		return State;
	}

	static FString FoliageTypeFingerprint(const UFoliageType* Type);

	static void CompleteReceipt(
		TSharedRef<FJsonObject>& Out,
		const FString& ToolName,
		const FString& TargetPath,
		int32 Before,
		int32 After,
		int32 Affected,
		bool bReadbackVerified = false,
		const FFoliageInfo* Info = nullptr,
		const UFoliageType* Type = nullptr)
	{
		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
		Out->SetBoolField(TEXT("python_used"), false);
		Out->SetStringField(TEXT("tool"), ToolName);
		Out->SetStringField(TEXT("target"), TargetPath);
		Out->SetNumberField(TEXT("instance_count_before"), Before);
		Out->SetNumberField(TEXT("instance_count_after"), After);
		Out->SetNumberField(TEXT("affected_instances"), Affected);
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("schema"), TEXT("somol.foliage_authoring_receipt.v1"));
		Receipt->SetStringField(TEXT("receipt_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		Receipt->SetStringField(TEXT("tool"), ToolName);
		Receipt->SetStringField(TEXT("target"), TargetPath);
		Receipt->SetStringField(TEXT("status"), TEXT("completed"));
		Receipt->SetNumberField(TEXT("instance_count_before"), Before);
		Receipt->SetNumberField(TEXT("instance_count_after"), After);
		Receipt->SetNumberField(TEXT("affected_instances"), Affected);
		Receipt->SetBoolField(TEXT("readback_verified"), bReadbackVerified);
		Receipt->SetStringField(TEXT("readback_verification_mode"), bReadbackVerified
			? (Out->HasField(TEXT("readback_verification_mode")) ? Out->GetStringField(TEXT("readback_verification_mode")) : TEXT("live_memory_readback"))
			: TEXT("unverified_dirty_memory_state"));
		if (Info)
		{
			Receipt->SetObjectField(TEXT("state_readback"), FoliageStateReadback(Info));
		}
		if (Type)
		{
			Receipt->SetStringField(TEXT("property_fingerprint_crc32"), FoliageTypeFingerprint(Type));
		}
		Receipt->SetBoolField(TEXT("python_used"), false);
		Out->SetObjectField(TEXT("receipt"), Receipt);
		FAtomicFoliageTransaction::CommitActive();
	}

	static bool ProjectToSurface(
		UWorld* World,
		const FVector& Candidate,
		double TraceHalfHeight,
		FVector& OutLocation,
		FVector& OutNormal)
	{
		if (!World || TraceHalfHeight <= 0.0)
		{
			return false;
		}
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(SomolmcpFoliageSurfaceProjection), true);
		const FVector Start = Candidate + FVector(0.0, 0.0, TraceHalfHeight);
		const FVector End = Candidate - FVector(0.0, 0.0, TraceHalfHeight);
		if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params) || !Hit.bBlockingHit)
		{
			return false;
		}
		OutLocation = Hit.ImpactPoint;
		OutNormal = Hit.ImpactNormal.GetSafeNormal();
		return OutNormal.IsNormalized();
	}

	static bool PassesSlope(const FVector& Normal, double MaxSlopeDegrees)
	{
		const double Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Normal.Z, -1.0, 1.0)));
		return Slope <= FMath::Clamp(MaxSlopeDegrees, 0.0, 89.9);
	}

	static bool HasNearbyInstance(const FFoliageInfo* Info, const FVector& Location, double SpacingCm, int32 IgnoreIndex = INDEX_NONE)
	{
		if (!Info || SpacingCm <= 0.0)
		{
			return false;
		}
		const double SpacingSquared = SpacingCm * SpacingCm;
		for (int32 Index = 0; Index < Info->Instances.Num(); ++Index)
		{
			if (Index != IgnoreIndex && FVector::DistSquared(Info->Instances[Index].Location, Location) < SpacingSquared)
			{
				return true;
			}
		}
		return false;
	}

	static bool HasDynamicCollision(UWorld* World, const FVector& Location, double RadiusCm)
	{
		if (!World || RadiusCm <= 0.0)
		{
			return false;
		}
		FCollisionQueryParams Params(SCENE_QUERY_STAT(SomolmcpFoliageCollisionFilter), false);
		return World->OverlapBlockingTestByChannel(
			Location,
			FQuat::Identity,
			ECC_WorldDynamic,
			FCollisionShape::MakeSphere(RadiusCm),
			Params);
	}

	static FFoliageInstance MakeInstance(
		const FVector& Location,
		const FVector& Normal,
		UFoliageType* Type,
		FRandomStream& Random,
		const TSharedRef<FJsonObject>& Arguments,
		UActorComponent* InBaseComponent = nullptr)
	{
		FFoliageInstance Instance;
		Instance.Location = Location;
		Instance.BaseComponent = InBaseComponent;
		const bool bRandomYaw = Boolean(Arguments, TEXT("random_yaw"), Type ? !!Type->RandomYaw : true);
		Instance.Rotation = FRotator(0.0, bRandomYaw ? Random.FRandRange(0.0f, 360.0f) : 0.0f, 0.0);
		const double DefaultMin = Type ? Type->ScaleX.Min : 1.0;
		const double DefaultMax = Type ? Type->ScaleX.Max : 1.0;
		const float ScaleMin = static_cast<float>(FMath::Max(0.001, Number(Arguments, TEXT("scale_min"), DefaultMin)));
		const float ScaleMax = static_cast<float>(FMath::Max<double>(ScaleMin, Number(Arguments, TEXT("scale_max"), DefaultMax)));
		const float Scale = Random.FRandRange(ScaleMin, ScaleMax);
		Instance.DrawScale3D = FVector3f(Scale);
		if (Boolean(Arguments, TEXT("align_to_normal"), Type ? !!Type->AlignToNormal : false))
		{
			Instance.AlignToNormal(Normal, static_cast<float>(Number(Arguments, TEXT("align_max_angle"), Type ? Type->AlignMaxAngle : 0.0)));
		}
		return Instance;
	}

	static bool PrepareCandidate(
		FFoliageTarget& Target,
		const TSharedRef<FJsonObject>& Arguments,
		const FVector& Candidate,
		FVector& OutLocation,
		FVector& OutNormal)
	{
		OutLocation = Candidate;
		OutNormal = FVector::UpVector;
		if (Boolean(Arguments, TEXT("project_to_surface"), true))
		{
			if (!ProjectToSurface(Target.World, Candidate, Number(Arguments, TEXT("trace_half_height_cm"), 100000.0), OutLocation, OutNormal))
			{
				return false;
			}
		}
		if (!PassesSlope(OutNormal, Number(Arguments, TEXT("max_slope_degrees"), Target.Type ? Target.Type->GroundSlopeAngle.Max : 45.0)))
		{
			return false;
		}
		const double Spacing = Number(Arguments, TEXT("spacing_cm"), Target.Type ? Target.Type->Radius : 0.0);
		if (HasNearbyInstance(Target.Info, OutLocation, Spacing))
		{
			return false;
		}
		if (Boolean(Arguments, TEXT("collision_avoidance"), Target.Type ? !!Target.Type->CollisionWithWorld : false)
			&& HasDynamicCollision(Target.World, OutLocation, Number(Arguments, TEXT("collision_radius_cm"), FMath::Max(1.0, Spacing * 0.5))))
		{
			return false;
		}
		return true;
	}

	static int32 AddCandidates(
		FFoliageTarget& Target,
		const TSharedRef<FJsonObject>& Arguments,
		const TArray<FVector>& Candidates,
		FRandomStream& Random,
		int32& Rejected)
	{
		int32 Added = 0;
		Rejected = 0;
		for (const FVector& Candidate : Candidates)
		{
			FVector Location, Normal;
			if (!PrepareCandidate(Target, Arguments, Candidate, Location, Normal))
			{
				++Rejected;
				continue;
			}
			Target.Info->AddInstance(Target.Type, MakeInstance(Location, Normal, Target.Type, Random, Arguments, Target.Actor->GetRootComponent()));
		}
		if (Added > 0)
		{
			Target.Info->Refresh(false, true);
		}
		return Added;
	}

	static void IndicesInSphere(const FFoliageInfo* Info, const FVector& Center, double Radius, TArray<int32>& Out)
	{
		Out.Reset();
		if (Info)
		{
			Info->GetInstancesInsideSphere(FSphere(Center, Radius), Out);
		}
	}

	static bool PointInPolygon2D(const FVector2D& Point, const TArray<FVector>& Polygon)
	{
		bool bInside = false;
		for (int32 I = 0, J = Polygon.Num() - 1; I < Polygon.Num(); J = I++)
		{
			const FVector2D A(Polygon[I].X, Polygon[I].Y);
			const FVector2D B(Polygon[J].X, Polygon[J].Y);
			const double Denominator = B.Y - A.Y;
			if (((A.Y > Point.Y) != (B.Y > Point.Y))
				&& Point.X < A.X + (B.X - A.X) * (Point.Y - A.Y) / Denominator)
			{
				bInside = !bInside;
			}
		}
		return bInside;
	}

	static void IndicesInPolygon(const FFoliageInfo* Info, const TArray<FVector>& Polygon, TArray<int32>& Out)
	{
		Out.Reset();
		if (!Info || Polygon.Num() < 3)
		{
			return;
		}
		for (int32 Index = 0; Index < Info->Instances.Num(); ++Index)
		{
			const FVector& Location = Info->Instances[Index].Location;
			if (PointInPolygon2D(FVector2D(Location.X, Location.Y), Polygon))
			{
				Out.Add(Index);
			}
		}
	}

	static bool PersistMutation(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		FFoliageTarget& Target,
		const FString& ToolName,
		int32 Before,
		int32 Affected,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		const int32 After = Target.Info ? Target.Info->Instances.Num() : 0;
		TArray<UObject*> MutationTargets{Target.Actor};
		if (Target.Info && Target.Info->GetComponent())
		{
			MutationTargets.Add(Target.Info->GetComponent());
		}
		if (!SaveWorld(Context, Target.World, MutationTargets, Boolean(Arguments, TEXT("save"), true), Out, Error))
		{
			return false;
		}
		bool bPersistenceVerified = false;
		Out->TryGetBoolField(TEXT("persistence_verified"), bPersistenceVerified);
		CompleteReceipt(Out, ToolName, Target.TypePath, Before, After, Affected, bPersistenceVerified, Target.Info, Target.Type);
		Summary = FString::Printf(TEXT("%s completed: before=%d after=%d affected=%d."), *ToolName, Before, After, Affected);
		return true;
	}

	static AActor* FindActor(UWorld* World, const FString& Identifier)
	{
		if (!World || Identifier.IsEmpty()) return nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && (Actor->GetName().Equals(Identifier, ESearchCase::IgnoreCase)
				|| Actor->GetActorLabel().Equals(Identifier, ESearchCase::IgnoreCase)
				|| Actor->GetPathName().Equals(Identifier, ESearchCase::IgnoreCase)))
			{
				return Actor;
			}
		}
		return nullptr;
	}

	static UInstancedStaticMeshComponent* FindISMComponent(AActor* Actor, const FString& Identifier, bool bRequireHISM, bool bRejectHISM)
	{
		if (!Actor) return nullptr;
		TInlineComponentArray<UInstancedStaticMeshComponent*> Components(Actor);
		for (UInstancedStaticMeshComponent* Component : Components)
		{
			if (!Component) continue;
			const bool bIsHISM = Component->IsA<UHierarchicalInstancedStaticMeshComponent>();
			if ((bRequireHISM && !bIsHISM) || (bRejectHISM && bIsHISM)) continue;
			if (Identifier.IsEmpty()
				|| Component->GetName().Equals(Identifier, ESearchCase::IgnoreCase)
				|| Component->GetPathName().Equals(Identifier, ESearchCase::IgnoreCase))
			{
				return Component;
			}
		}
		return nullptr;
	}

	static void NotifyFoliageTypeChanged(UWorld* World, UFoliageType* Type, bool bSourceChanged)
	{
		if (!World || !Type) return;
		// AInstancedFoliageActor::NotifyFoliageTypeChanged is declared on UE 5.3 but
		// only gained FOLIAGE_API in 5.4, so calling it from outside the Foliage
		// module is a link error there. PostEditChange is the public path the
		// foliage editor itself relies on to propagate a type change; the instance
		// refresh it triggers is the same one, just routed through the type.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
		for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
		{
			It->NotifyFoliageTypeChanged(Type, bSourceChanged);
		}
#else
		(void)bSourceChanged;
		Type->PostEditChange();
#endif
	}

	static TSharedRef<FJsonObject> FoliageTypePropertiesJson(const UFoliageType* Type)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Type) return Out;
		Out->SetNumberField(TEXT("density"), Type->Density);
		Out->SetNumberField(TEXT("density_adjustment_factor"), Type->DensityAdjustmentFactor);
		Out->SetNumberField(TEXT("radius"), Type->Radius);
		Out->SetBoolField(TEXT("single_instance_override_radius"), Type->bSingleInstanceModeOverrideRadius);
		Out->SetNumberField(TEXT("single_instance_radius"), Type->SingleInstanceModeRadius);
		Out->SetObjectField(TEXT("scale_min"), VectorJson(FVector(Type->ScaleX.Min, Type->ScaleY.Min, Type->ScaleZ.Min)));
		Out->SetObjectField(TEXT("scale_max"), VectorJson(FVector(Type->ScaleX.Max, Type->ScaleY.Max, Type->ScaleZ.Max)));
		Out->SetNumberField(TEXT("z_offset_min"), Type->ZOffset.Min);
		Out->SetNumberField(TEXT("z_offset_max"), Type->ZOffset.Max);
		Out->SetBoolField(TEXT("align_to_normal"), !!Type->AlignToNormal);
		Out->SetNumberField(TEXT("align_max_angle"), Type->AlignMaxAngle);
		Out->SetBoolField(TEXT("random_yaw"), !!Type->RandomYaw);
		Out->SetNumberField(TEXT("random_pitch_angle"), Type->RandomPitchAngle);
		Out->SetNumberField(TEXT("slope_min"), Type->GroundSlopeAngle.Min);
		Out->SetNumberField(TEXT("slope_max"), Type->GroundSlopeAngle.Max);
		Out->SetNumberField(TEXT("height_min"), Type->Height.Min);
		Out->SetNumberField(TEXT("height_max"), Type->Height.Max);
		Out->SetBoolField(TEXT("collision_with_world"), !!Type->CollisionWithWorld);
		Out->SetObjectField(TEXT("collision_scale"), VectorJson(Type->CollisionScale));
		Out->SetNumberField(TEXT("minimum_layer_weight"), Type->MinimumLayerWeight);
		Out->SetNumberField(TEXT("cull_start"), Type->CullDistance.Min);
		Out->SetNumberField(TEXT("cull_end"), Type->CullDistance.Max);
		Out->SetNumberField(TEXT("mobility"), static_cast<int32>(Type->Mobility.GetValue()));
		Out->SetBoolField(TEXT("cast_shadow"), !!Type->CastShadow);
		Out->SetBoolField(TEXT("cast_dynamic_shadow"), !!Type->bCastDynamicShadow);
		Out->SetBoolField(TEXT("cast_static_shadow"), !!Type->bCastStaticShadow);
		Out->SetBoolField(TEXT("visible_in_ray_tracing"), !!Type->bVisibleInRayTracing);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		Out->SetBoolField(TEXT("visible_in_reflections"), !!Type->bVisibleInReflections);
		Out->SetNumberField(TEXT("nanite_pixel_programmable_distance"), Type->NanitePixelProgrammableDistance);
#endif
		Out->SetBoolField(TEXT("evaluate_world_position_offset"), !!Type->bEvaluateWorldPositionOffset);
		Out->SetNumberField(TEXT("world_position_offset_disable_distance"), Type->WorldPositionOffsetDisableDistance);
		Out->SetBoolField(TEXT("render_custom_depth"), !!Type->bRenderCustomDepth);
		Out->SetNumberField(TEXT("custom_depth_stencil_value"), Type->CustomDepthStencilValue);
		Out->SetBoolField(TEXT("enable_density_scaling"), !!Type->bEnableDensityScaling);
		Out->SetBoolField(TEXT("enable_cull_distance_scaling"), !!Type->bEnableCullDistanceScaling);
		Out->SetNumberField(TEXT("virtual_texture_cull_mips"), Type->VirtualTextureCullMips);
#if WITH_EDITORONLY_DATA
		Out->SetBoolField(TEXT("include_in_hlod"), !!Type->bIncludeInHLOD);
#endif
		return Out;
	}

	static FString FoliageTypeFingerprint(const UFoliageType* Type)
	{
		if (!Type)
		{
			return FString::Printf(TEXT("%08X"), FCrc::StrCrc32(TEXT("null_foliage_type")));
		}
		FString Canonical;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Canonical);
		FJsonSerializer::Serialize(FoliageTypePropertiesJson(Type), Writer);
		Canonical += TEXT("|") + Type->GetPathName();
		Canonical += TEXT("|") + Type->UpdateGuid.ToString(EGuidFormats::Digits);
		if (const UFoliageType_InstancedStaticMesh* MeshType = Cast<UFoliageType_InstancedStaticMesh>(Type))
		{
			Canonical += TEXT("|") + (MeshType->GetStaticMesh() ? MeshType->GetStaticMesh()->GetPathName() : FString());
		}
		return FString::Printf(TEXT("%08X"), FCrc::StrCrc32(*Canonical));
	}

	static void RegisterTool(
		FSololmcpToolRegistry& Registry,
		const FString& Name,
		const FString& Description,
		const TArray<FString>& Required,
		FHandler Handler)
	{
		FSololmcpToolDefinition Definition;
		Definition.Name = Name;
		Definition.Description = Description;
		Definition.InputSchema = FoliageAuthorSchema(Required);
		Definition.Execute = [Name, Handler = MoveTemp(Handler)](
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& Out,
			FString& Summary,
			FString& Error) mutable
		{
			FAtomicFoliageTransaction WholeToolTransaction(
				FText::FromString(FString::Printf(TEXT("SOMOLMCP atomic %s"), *Name)));
			const bool bReadOnly = Name == TEXT("foliage_author_type_get_properties");
			if (!bReadOnly && !WholeToolTransaction.IsOpen())
			{
				Fail(Out, Error, TEXT("transaction_unavailable"), TEXT("A rollback-capable editor transaction is required for foliage writes."));
				return false;
			}
			const bool bSucceeded = Handler(Context, Arguments, Out, Summary, Error);
			if (bSucceeded)
			{
				WholeToolTransaction.Commit();
			}
			else if (!bReadOnly)
			{
				const bool bRollbackVerified = WholeToolTransaction.Cancel();
				Out->SetBoolField(TEXT("memory_rollback_verified"), bRollbackVerified);
				if (!bRollbackVerified)
				{
					Out->SetStringField(TEXT("error_code"), TEXT("memory_rollback_failed"));
					Error = Error.IsEmpty() ? TEXT("The failed foliage write could not be undone.") : Error + TEXT(" The editor transaction could not be undone.");
					Out->SetStringField(TEXT("error"), Error);
				}
			}
			return bSucceeded;
		};
		Definition.bUsesExternalPython = false;
		Registry.Register(Definition);
	}
}

void RegisterFoliageCompletionTools(FSololmcpToolRegistry& Registry)
{
	RegisterTool(Registry, TEXT("foliage_author_paint"),
		TEXT("Paint deterministic native foliage instances inside a circular brush with surface, slope, spacing, and collision filters."),
		{TEXT("foliage_type_path"), TEXT("center"), TEXT("count")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			FVector Center;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), true, Target, Out, Error) || !TryVector(Arguments, TEXT("center"), Center))
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("invalid_center"), TEXT("center must be a finite vector object."));
				return false;
			}
			const int32 Count = Integer(Arguments, TEXT("count"), 0);
			if (Count <= 0 || Count > 100000)
			{
				Fail(Out, Error, TEXT("invalid_count"), TEXT("count must be in [1, 100000]."));
				return false;
			}
			const double Radius = FMath::Max(0.0, Number(Arguments, TEXT("radius_cm"), 500.0));
			FRandomStream Random(Integer(Arguments, TEXT("seed"), 1337));
			TArray<FVector> Candidates;
			Candidates.Reserve(Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const double Angle = Random.FRandRange(0.0f, 2.0f * PI);
				const double Distance = FMath::Sqrt(Random.FRand()) * Radius;
				Candidates.Add(Center + FVector(FMath::Cos(Angle) * Distance, FMath::Sin(Angle) * Distance, 0.0));
			}
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Paint")));
			Target.Actor->Modify();
			if (UActorComponent* Component = Target.Info->GetComponent()) Component->Modify();
			int32 Rejected = 0;
			const int32 Added = AddCandidates(Target, Arguments, Candidates, Random, Rejected);
			Out->SetNumberField(TEXT("rejected_candidates"), Rejected);
			if (Added <= 0)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("no_valid_placements"), TEXT("All paint candidates were rejected by projection, slope, spacing, or collision filters."));
				return false;
			}
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_paint"), Before, Added, Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_erase"),
		TEXT("Erase native foliage instances inside a spherical brush and return exact count readback."),
		{TEXT("foliage_type_path"), TEXT("center"), TEXT("radius_cm")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			FVector Center;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), false, Target, Out, Error) || !TryVector(Arguments, TEXT("center"), Center))
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("invalid_center"), TEXT("center must be a finite vector object."));
				return false;
			}
			if (!Target.Info)
			{
				Fail(Out, Error, TEXT("foliage_type_not_in_world"), TEXT("The requested FoliageType has no instances in the current level."));
				return false;
			}
			TArray<int32> Indices;
			IndicesInSphere(Target.Info, Center, FMath::Max(0.0, Number(Arguments, TEXT("radius_cm"), 0.0)), Indices);
			if (Indices.IsEmpty())
			{
				Fail(Out, Error, TEXT("no_matching_instances"), TEXT("No foliage instances intersect the erase brush."));
				return false;
			}
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Erase")));
			Target.Actor->Modify();
			Target.Info->RemoveInstances(Indices, true);
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_erase"), Before, Indices.Num(), Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_smooth"),
		TEXT("Smooth foliage positions toward their regional centroid with bounded strength and transform readback."),
		{TEXT("foliage_type_path"), TEXT("center"), TEXT("radius_cm"), TEXT("strength")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			FVector Center;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), false, Target, Out, Error) || !TryVector(Arguments, TEXT("center"), Center) || !Target.Info)
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("invalid_target"), TEXT("A valid center and existing foliage instances are required."));
				return false;
			}
			TArray<int32> Indices;
			IndicesInSphere(Target.Info, Center, FMath::Max(0.0, Number(Arguments, TEXT("radius_cm"), 0.0)), Indices);
			if (Indices.Num() < 2)
			{
				Fail(Out, Error, TEXT("insufficient_instances"), TEXT("Smoothing requires at least two instances inside the brush."));
				return false;
			}
			FVector Centroid = FVector::ZeroVector;
			for (int32 Index : Indices) Centroid += Target.Info->Instances[Index].Location;
			Centroid /= static_cast<double>(Indices.Num());
			const double Strength = FMath::Clamp(Number(Arguments, TEXT("strength"), 0.25), 0.0, 1.0);
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Smooth")));
			Target.Actor->Modify();
			Target.Info->PreMoveInstances(Indices);
			for (int32 Index : Indices)
			{
				FTransform Transform = Target.Info->Instances[Index].GetInstanceWorldTransform();
				FVector NewLocation = FMath::Lerp(Transform.GetLocation(), FVector(Centroid.X, Centroid.Y, Transform.GetLocation().Z), Strength);
				Transform.SetLocation(NewLocation);
				Target.Info->SetInstanceWorldTransform(Index, Transform, true);
			}
			Target.Info->PostMoveInstances(Indices, true);
			Target.Info->Refresh(false, true);
			Out->SetObjectField(TEXT("centroid"), VectorJson(Centroid));
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_smooth"), Before, Indices.Num(), Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_fill"),
		TEXT("Fill an axis-aligned world region with native foliage using deterministic density and placement filters."),
		{TEXT("foliage_type_path"), TEXT("min"), TEXT("max")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			FVector Min, Max;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), true, Target, Out, Error)
				|| !TryVector(Arguments, TEXT("min"), Min) || !TryVector(Arguments, TEXT("max"), Max))
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("invalid_bounds"), TEXT("min and max must be finite vector objects."));
				return false;
			}
			const FBox Bounds(Min.ComponentMin(Max), Min.ComponentMax(Max));
			if (!Bounds.IsValid || Bounds.GetSize().X <= 0.0 || Bounds.GetSize().Y <= 0.0)
			{
				Fail(Out, Error, TEXT("invalid_bounds"), TEXT("Fill bounds must have non-zero X/Y extent."));
				return false;
			}
			const double DensityPerSquareMeter = FMath::Max(0.0, Number(Arguments, TEXT("density"), Target.Type->Density / 100.0));
			int32 Count = Integer(Arguments, TEXT("count"), FMath::CeilToInt((Bounds.GetSize().X * Bounds.GetSize().Y / 10000.0) * DensityPerSquareMeter));
			if (Count <= 0 || Count > 100000)
			{
				Fail(Out, Error, TEXT("invalid_fill_count"), TEXT("Derived or explicit fill count must be in [1, 100000]."));
				return false;
			}
			FRandomStream Random(Integer(Arguments, TEXT("seed"), 1337));
			TArray<FVector> Candidates;
			Candidates.Reserve(Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Candidates.Add(FVector(Random.FRandRange(Bounds.Min.X, Bounds.Max.X), Random.FRandRange(Bounds.Min.Y, Bounds.Max.Y), (Bounds.Min.Z + Bounds.Max.Z) * 0.5));
			}
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Fill")));
			Target.Actor->Modify();
			int32 Rejected = 0;
			const int32 Added = AddCandidates(Target, Arguments, Candidates, Random, Rejected);
			Out->SetNumberField(TEXT("rejected_candidates"), Rejected);
			if (Added <= 0)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("no_valid_placements"), TEXT("All fill candidates were rejected by the placement filters."));
				return false;
			}
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_fill"), Before, Added, Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_reapply"),
		TEXT("Reapply enabled FoliageType scale, yaw, alignment, height, slope, spacing, and collision rules to placed instances."),
		{TEXT("foliage_type_path")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), false, Target, Out, Error) || !Target.Info)
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("no_instances"), TEXT("The FoliageType has no instances to reapply."));
				return false;
			}
			const int32 Before = Target.Info->Instances.Num();
			FRandomStream Random(Integer(Arguments, TEXT("seed"), Target.Type->DistributionSeed));
			TArray<int32> ToRemove;
			TArray<TPair<int32, FTransform>> TransformUpdates;
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Reapply")));
			Target.Actor->Modify();
			for (int32 Index = 0; Index < Target.Info->Instances.Num(); ++Index)
			{
				const FFoliageInstance& Instance = Target.Info->Instances[Index];
				FVector SurfaceLocation, SurfaceNormal;
				const bool bHasSurface = ProjectToSurface(Target.World, Instance.Location, Number(Arguments, TEXT("trace_half_height_cm"), 100000.0), SurfaceLocation, SurfaceNormal);
				if ((Target.Type->ReapplyHeight && (Instance.Location.Z < Target.Type->Height.Min || Instance.Location.Z > Target.Type->Height.Max))
					|| (Target.Type->ReapplyGroundSlope && (!bHasSurface || !PassesSlope(SurfaceNormal, Target.Type->GroundSlopeAngle.Max)))
					|| (Target.Type->ReapplyCollisionWithWorld && HasDynamicCollision(Target.World, Instance.Location, FMath::Max(1.0f, Target.Type->Radius * 0.5f))))
				{
					ToRemove.Add(Index);
					continue;
				}
				const FTransform TransformBefore = Instance.GetInstanceWorldTransform();
				FFoliageInstance Updated = Instance;
				if (Target.Type->ReapplyAlignToNormal && bHasSurface)
				{
					Updated.AlignToNormal(SurfaceNormal, Target.Type->AlignMaxAngle);
				}
				if (Target.Type->ReapplyRandomYaw)
				{
					Updated.Rotation.Yaw = Target.Type->RandomYaw ? Random.FRandRange(0.0f, 360.0f) : 0.0f;
				}
				if (Target.Type->ReapplyScaling || Target.Type->ReapplyScaleX || Target.Type->ReapplyScaleY || Target.Type->ReapplyScaleZ)
				{
					FVector3f Scale = Updated.DrawScale3D;
					if (Target.Type->ReapplyScaling || Target.Type->ReapplyScaleX) Scale.X = Random.FRandRange(Target.Type->ScaleX.Min, Target.Type->ScaleX.Max);
					if (Target.Type->ReapplyScaling || Target.Type->ReapplyScaleY) Scale.Y = Random.FRandRange(Target.Type->ScaleY.Min, Target.Type->ScaleY.Max);
					if (Target.Type->ReapplyScaling || Target.Type->ReapplyScaleZ) Scale.Z = Random.FRandRange(Target.Type->ScaleZ.Min, Target.Type->ScaleZ.Max);
					Updated.DrawScale3D = Scale;
				}
				const FTransform TransformAfter = Updated.GetInstanceWorldTransform();
				if (!TransformBefore.Equals(TransformAfter, KINDA_SMALL_NUMBER))
				{
					TransformUpdates.Emplace(Index, TransformAfter);
				}
			}
			TArray<int32> ToMove;
			ToMove.Reserve(TransformUpdates.Num());
			for (const TPair<int32, FTransform>& Update : TransformUpdates)
			{
				ToMove.Add(Update.Key);
			}
			if (!ToMove.IsEmpty())
			{
				Target.Info->PreMoveInstances(ToMove);
				for (const TPair<int32, FTransform>& Update : TransformUpdates)
				{
					Target.Info->SetInstanceWorldTransform(Update.Key, Update.Value, true);
				}
				Target.Info->PostMoveInstances(ToMove, true);
			}
			if (!ToRemove.IsEmpty()) Target.Info->RemoveInstances(ToRemove, true);
			if (!ToMove.IsEmpty() || !ToRemove.IsEmpty()) Target.Info->Refresh(false, true);
			const int32 Affected = ToMove.Num() + ToRemove.Num();
			if (Affected <= 0)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("no_reapply_rules_enabled"), TEXT("No enabled reapply setting changed or removed an instance."));
				return false;
			}
			Out->SetNumberField(TEXT("removed_instances"), ToRemove.Num());
			Out->SetNumberField(TEXT("updated_instances"), ToMove.Num());
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_reapply"), Before, Affected, Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_lasso_erase"),
		TEXT("Erase FoliageType instances whose XY positions lie inside a lasso polygon."),
		{TEXT("foliage_type_path"), TEXT("points")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			TArray<FVector> Polygon;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), false, Target, Out, Error)
				|| !TryPoints(Arguments, TEXT("points"), Polygon) || Polygon.Num() < 3 || !Target.Info)
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("invalid_lasso"), TEXT("An existing FoliageType and at least three polygon points are required."));
				return false;
			}
			TArray<int32> Indices;
			IndicesInPolygon(Target.Info, Polygon, Indices);
			if (Indices.IsEmpty())
			{
				Fail(Out, Error, TEXT("no_matching_instances"), TEXT("No instances lie inside the lasso polygon."));
				return false;
			}
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Lasso Erase")));
			Target.Actor->Modify();
			Target.Info->RemoveInstances(Indices, true);
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_lasso_erase"), Before, Indices.Num(), Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_polygon_fill"),
		TEXT("Fill a polygon with deterministic surface-projected foliage and bounded candidate count."),
		{TEXT("foliage_type_path"), TEXT("points")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			TArray<FVector> Polygon;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), true, Target, Out, Error)
				|| !TryPoints(Arguments, TEXT("points"), Polygon) || Polygon.Num() < 3)
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("invalid_polygon"), TEXT("At least three polygon points are required."));
				return false;
			}
			FBox2D Bounds(ForceInit);
			double Z = 0.0;
			for (const FVector& Point : Polygon) { Bounds += FVector2D(Point.X, Point.Y); Z += Point.Z; }
			Z /= Polygon.Num();
			const double AreaSquareMeters = (Bounds.GetSize().X * Bounds.GetSize().Y) / 10000.0;
			int32 Count = Integer(Arguments, TEXT("count"), FMath::CeilToInt(AreaSquareMeters * FMath::Max(0.0, Number(Arguments, TEXT("density"), Target.Type->Density / 100.0))));
			if (Count <= 0 || Count > 100000)
			{
				Fail(Out, Error, TEXT("invalid_fill_count"), TEXT("Polygon fill count must be in [1, 100000]."));
				return false;
			}
			FRandomStream Random(Integer(Arguments, TEXT("seed"), 1337));
			TArray<FVector> Candidates;
			for (int32 Attempt = 0; Attempt < Count * 4 && Candidates.Num() < Count; ++Attempt)
			{
				FVector Candidate(Random.FRandRange(Bounds.Min.X, Bounds.Max.X), Random.FRandRange(Bounds.Min.Y, Bounds.Max.Y), Z);
				if (PointInPolygon2D(FVector2D(Candidate.X, Candidate.Y), Polygon)) Candidates.Add(Candidate);
			}
			if (Candidates.IsEmpty())
			{
				Fail(Out, Error, TEXT("polygon_sampling_failed"), TEXT("No candidate could be sampled inside the polygon."));
				return false;
			}
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Polygon Fill")));
			Target.Actor->Modify();
			int32 Rejected = 0;
			const int32 Added = AddCandidates(Target, Arguments, Candidates, Random, Rejected);
			Out->SetNumberField(TEXT("rejected_candidates"), Rejected);
			if (Added <= 0)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("no_valid_placements"), TEXT("All polygon candidates were rejected by placement filters."));
				return false;
			}
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_polygon_fill"), Before, Added, Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_spline_paint"),
		TEXT("Paint foliage along a polyline/spline corridor using spacing and width controls."),
		{TEXT("foliage_type_path"), TEXT("points")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			TArray<FVector> Points;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), true, Target, Out, Error)
				|| !TryPoints(Arguments, TEXT("points"), Points) || Points.Num() < 2)
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("invalid_spline"), TEXT("At least two spline points are required."));
				return false;
			}
			const double Spacing = FMath::Max(1.0, Number(Arguments, TEXT("spacing_cm"), FMath::Max(1.0f, Target.Type->Radius)));
			const double Width = FMath::Max(0.0, Number(Arguments, TEXT("width_cm"), 0.0));
			FRandomStream Random(Integer(Arguments, TEXT("seed"), 1337));
			TArray<FVector> Candidates;
			for (int32 Segment = 0; Segment + 1 < Points.Num(); ++Segment)
			{
				const FVector Delta = Points[Segment + 1] - Points[Segment];
				const double Length = Delta.Size();
				if (Length <= KINDA_SMALL_NUMBER) continue;
				const FVector Direction = Delta / Length;
				const FVector Side(-Direction.Y, Direction.X, 0.0);
				for (double Distance = Segment == 0 ? 0.0 : Spacing; Distance <= Length; Distance += Spacing)
				{
					Candidates.Add(Points[Segment] + Direction * Distance + Side * Random.FRandRange(-Width * 0.5, Width * 0.5));
					if (Candidates.Num() > 100000)
					{
						Fail(Out, Error, TEXT("candidate_limit"), TEXT("Spline paint exceeds the 100000 candidate safety limit."));
						return false;
					}
				}
			}
			if (Candidates.IsEmpty())
			{
				Fail(Out, Error, TEXT("empty_spline"), TEXT("The spline did not produce any paint candidates."));
				return false;
			}
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Spline Paint")));
			Target.Actor->Modify();
			int32 Rejected = 0;
			const int32 Added = AddCandidates(Target, Arguments, Candidates, Random, Rejected);
			Out->SetNumberField(TEXT("rejected_candidates"), Rejected);
			if (Added <= 0)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("no_valid_placements"), TEXT("All spline candidates were rejected by placement filters."));
				return false;
			}
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_spline_paint"), Before, Added, Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_single_add"),
		TEXT("Add one native foliage instance with explicit transform, optional projection, and placement validation."),
		{TEXT("foliage_type_path"), TEXT("location")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			FVector Candidate;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), true, Target, Out, Error) || !TryVector(Arguments, TEXT("location"), Candidate))
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("invalid_location"), TEXT("location must be a finite vector object."));
				return false;
			}
			FVector Location, Normal;
			if (!PrepareCandidate(Target, Arguments, Candidate, Location, Normal))
			{
				Fail(Out, Error, TEXT("placement_rejected"), TEXT("The single instance was rejected by projection, slope, spacing, or collision filters."));
				return false;
			}
			FRandomStream Random(Integer(Arguments, TEXT("seed"), 1337));
			FFoliageInstance Instance = MakeInstance(Location, Normal, Target.Type, Random, Arguments);
			Instance.BaseComponent = Target.Actor->GetRootComponent();
			FVector Rotation, Scale;
			if (TryVector(Arguments, TEXT("rotation"), Rotation)) Instance.Rotation = FRotator(Rotation.X, Rotation.Y, Rotation.Z);
			if (TryVector(Arguments, TEXT("scale"), Scale) && Scale.GetMin() > 0.0) Instance.DrawScale3D = FVector3f(Scale);
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Single Add")));
			Target.Actor->Modify();
			Target.Info->AddInstance(Target.Type, Instance);
			Target.Info->Refresh(false, true);
			Out->SetNumberField(TEXT("instance_index"), Target.Info->Instances.Num() - 1);
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_single_add"), Before, 1, Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_single_remove"),
		TEXT("Remove one foliage instance by stable editor index with bounds validation and count readback."),
		{TEXT("foliage_type_path"), TEXT("instance_index")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), false, Target, Out, Error) || !Target.Info)
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("no_instances"), TEXT("The requested FoliageType has no instances."));
				return false;
			}
			const int32 Index = Integer(Arguments, TEXT("instance_index"), INDEX_NONE);
			if (!Target.Info->Instances.IsValidIndex(Index))
			{
				Fail(Out, Error, TEXT("invalid_instance_index"), FString::Printf(TEXT("instance_index %d is outside [0, %d)."), Index, Target.Info->Instances.Num()));
				return false;
			}
			const int32 Before = Target.Info->Instances.Num();
			const TArray<int32> Indices{Index};
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Single Remove")));
			Target.Actor->Modify();
			Target.Info->RemoveInstances(Indices, true);
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_single_remove"), Before, 1, Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_single_transform"),
		TEXT("Set one foliage instance world transform and return the persisted transform readback."),
		{TEXT("foliage_type_path"), TEXT("instance_index")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), false, Target, Out, Error) || !Target.Info)
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("no_instances"), TEXT("The requested FoliageType has no instances."));
				return false;
			}
			const int32 Index = Integer(Arguments, TEXT("instance_index"), INDEX_NONE);
			if (!Target.Info->Instances.IsValidIndex(Index))
			{
				Fail(Out, Error, TEXT("invalid_instance_index"), TEXT("instance_index is outside the foliage instance array."));
				return false;
			}
			FTransform Transform = Target.Info->Instances[Index].GetInstanceWorldTransform();
			FVector Value;
			bool bChanged = false;
			if (TryVector(Arguments, TEXT("location"), Value)) { Transform.SetLocation(Value); bChanged = true; }
			if (TryVector(Arguments, TEXT("rotation"), Value)) { Transform.SetRotation(FRotator(Value.X, Value.Y, Value.Z).Quaternion()); bChanged = true; }
			if (TryVector(Arguments, TEXT("scale"), Value))
			{
				if (Value.GetMin() <= 0.0) { Fail(Out, Error, TEXT("invalid_scale"), TEXT("All scale axes must be greater than zero.")); return false; }
				Transform.SetScale3D(Value); bChanged = true;
			}
			if (!bChanged)
			{
				Fail(Out, Error, TEXT("missing_transform"), TEXT("At least one of location, rotation, or scale is required."));
				return false;
			}
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Single Transform")));
			Target.Actor->Modify();
			const TArray<int32> Indices{Index};
			Target.Info->PreMoveInstances(Indices);
			Target.Info->SetInstanceWorldTransform(Index, Transform, true);
			Target.Info->PostMoveInstances(Indices, true);
			Target.Info->Refresh(false, true);
			const FTransform Readback = Target.Info->Instances[Index].GetInstanceWorldTransform();
			Out->SetObjectField(TEXT("location_readback"), VectorJson(Readback.GetLocation()));
			Out->SetObjectField(TEXT("scale_readback"), VectorJson(Readback.GetScale3D()));
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_single_transform"), Before, 1, Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_replace_type"),
		TEXT("Replace source FoliageType instances with another FoliageType while preserving world transforms."),
		{TEXT("source_foliage_type_path"), TEXT("target_foliage_type_path")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Source, Destination;
			if (!ResolveTarget(Context, Arguments, TEXT("source_foliage_type_path"), false, Source, Out, Error)
				|| !ResolveTarget(Context, Arguments, TEXT("target_foliage_type_path"), true, Destination, Out, Error)
				|| !Source.Info)
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("invalid_replace_target"), TEXT("Both source and target foliage types and source instances are required."));
				return false;
			}
			if (Source.Type == Destination.Type)
			{
				Fail(Out, Error, TEXT("same_foliage_type"), TEXT("Source and target FoliageType assets must differ."));
				return false;
			}
			TArray<int32> SourceIndices;
			FVector Center;
			if (TryVector(Arguments, TEXT("center"), Center))
			{
				IndicesInSphere(Source.Info, Center, FMath::Max(0.0, Number(Arguments, TEXT("radius_cm"), 0.0)), SourceIndices);
			}
			else
			{
				for (int32 Index = 0; Index < Source.Info->Instances.Num(); ++Index) SourceIndices.Add(Index);
			}
			if (SourceIndices.IsEmpty())
			{
				Fail(Out, Error, TEXT("no_matching_instances"), TEXT("No source instances matched the replacement region."));
				return false;
			}
			const int32 SourceBefore = Source.Info->Instances.Num();
			const int32 DestinationBefore = Destination.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Replace Type")));
			Source.Actor->Modify();
			Destination.Actor->Modify();
			for (int32 Index : SourceIndices)
			{
				FFoliageInstance Replacement;
				Replacement.SetInstanceWorldTransform(Source.Info->Instances[Index].GetInstanceWorldTransform());
				Replacement.BaseComponent = Destination.Actor->GetRootComponent();
				Destination.Info->AddInstance(Destination.Type, Replacement);
			}
			Source.Info->RemoveInstances(SourceIndices, true);
			Destination.Info->Refresh(false, true);
			TArray<UObject*> MutationTargets{Source.Actor, Destination.Actor, Source.Info->GetComponent(), Destination.Info->GetComponent()};
			if (!SaveWorld(Context, Source.World, MutationTargets, Boolean(Arguments, TEXT("save"), true), Out, Error)) return false;
			bool bPersistenceVerified = false;
			Out->TryGetBoolField(TEXT("persistence_verified"), bPersistenceVerified);
			CompleteReceipt(Out, TEXT("foliage_author_replace_type"), Source.TypePath + TEXT(" -> ") + Destination.TypePath, SourceBefore + DestinationBefore, Source.Info->Instances.Num() + Destination.Info->Instances.Num(), SourceIndices.Num(), bPersistenceVerified, Destination.Info, Destination.Type);
			Out->SetNumberField(TEXT("source_count_after"), Source.Info->Instances.Num());
			Out->SetNumberField(TEXT("target_count_after"), Destination.Info->Instances.Num());
			Summary = FString::Printf(TEXT("Replaced %d foliage instances from %s with %s."), SourceIndices.Num(), *Source.TypePath, *Destination.TypePath);
			return true;
		});

	RegisterTool(Registry, TEXT("foliage_author_density_resample"),
		TEXT("Resample foliage density inside a spherical region to an exact target count with rollback on placement failure."),
		{TEXT("foliage_type_path"), TEXT("center"), TEXT("radius_cm"), TEXT("target_count")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			FVector Center;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), true, Target, Out, Error) || !TryVector(Arguments, TEXT("center"), Center))
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("invalid_center"), TEXT("center must be a finite vector object."));
				return false;
			}
			const double Radius = FMath::Max(0.0, Number(Arguments, TEXT("radius_cm"), 0.0));
			const int32 Desired = Integer(Arguments, TEXT("target_count"), INDEX_NONE);
			if (Desired < 0 || Desired > 100000)
			{
				Fail(Out, Error, TEXT("invalid_target_count"), TEXT("target_count must be in [0, 100000]."));
				return false;
			}
			TArray<int32> Existing;
			IndicesInSphere(Target.Info, Center, Radius, Existing);
			if (Existing.Num() == Desired)
			{
				Fail(Out, Error, TEXT("density_already_satisfied"), TEXT("The region already has the requested target_count; no write was performed."));
				return false;
			}
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Density Resample")));
			Target.Actor->Modify();
			int32 Affected = 0;
			if (Existing.Num() > Desired)
			{
				FRandomStream Random(Integer(Arguments, TEXT("seed"), 1337));
				for (int32 Index = Existing.Num() - 1; Index > 0; --Index) Existing.Swap(Index, Random.RandRange(0, Index));
				Existing.SetNum(Existing.Num() - Desired);
				Affected = Existing.Num();
				Target.Info->RemoveInstances(Existing, true);
			}
			else
			{
				const int32 Needed = Desired - Existing.Num();
				FRandomStream Random(Integer(Arguments, TEXT("seed"), 1337));
				TArray<FVector> Candidates;
				Candidates.Reserve(Needed * 8);
				for (int32 Attempt = 0; Attempt < Needed * 8; ++Attempt)
				{
					const double Angle = Random.FRandRange(0.0f, 2.0f * PI);
					const double Distance = FMath::Sqrt(Random.FRand()) * Radius;
					Candidates.Add(Center + FVector(FMath::Cos(Angle) * Distance, FMath::Sin(Angle) * Distance, 0.0));
				}
				int32 Rejected = 0;
				for (const FVector& Candidate : Candidates)
				{
					if (Affected >= Needed) break;
					FVector Location, Normal;
					if (!PrepareCandidate(Target, Arguments, Candidate, Location, Normal)) { ++Rejected; continue; }
					Target.Info->AddInstance(Target.Type, MakeInstance(Location, Normal, Target.Type, Random, Arguments, Target.Actor->GetRootComponent()));
					++Affected;
				}
				if (Affected != Needed)
				{
					TArray<int32> RollbackIndices;
					for (int32 Index = Before; Index < Target.Info->Instances.Num(); ++Index) RollbackIndices.Add(Index);
					if (!RollbackIndices.IsEmpty()) Target.Info->RemoveInstances(RollbackIndices, true);
					Transaction.Cancel();
					Fail(Out, Error, TEXT("target_density_unreachable"), FString::Printf(TEXT("Only %d/%d required instances passed placement filters; additions were rolled back."), Affected, Needed));
					return false;
				}
			}
			Target.Info->Refresh(false, true);
			TArray<int32> Readback;
			IndicesInSphere(Target.Info, Center, Radius, Readback);
			if (Readback.Num() != Desired)
			{
				Fail(Out, Error, TEXT("density_readback_mismatch"), FString::Printf(TEXT("Expected %d regional instances, read back %d."), Desired, Readback.Num()));
				return false;
			}
			Out->SetNumberField(TEXT("regional_count_after"), Readback.Num());
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_density_resample"), Before, Affected, Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_align_to_surface"),
		TEXT("Project and align foliage instances in a brush region to traced world-static surface normals."),
		{TEXT("foliage_type_path"), TEXT("center"), TEXT("radius_cm")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			FVector Center;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), false, Target, Out, Error) || !TryVector(Arguments, TEXT("center"), Center) || !Target.Info)
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("invalid_target"), TEXT("Existing foliage and a valid center are required."));
				return false;
			}
			TArray<int32> Indices;
			IndicesInSphere(Target.Info, Center, FMath::Max(0.0, Number(Arguments, TEXT("radius_cm"), 0.0)), Indices);
			if (Indices.IsEmpty()) { Fail(Out, Error, TEXT("no_matching_instances"), TEXT("No instances intersect the alignment brush.")); return false; }
			const int32 Before = Target.Info->Instances.Num();
			int32 Aligned = 0;
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Align To Surface")));
			Target.Actor->Modify();
			Target.Info->PreMoveInstances(Indices);
			for (int32 Index : Indices)
			{
				FVector Location, Normal;
				if (!ProjectToSurface(Target.World, Target.Info->Instances[Index].Location, Number(Arguments, TEXT("trace_half_height_cm"), 100000.0), Location, Normal)) continue;
				FFoliageInstance Instance = Target.Info->Instances[Index];
				Instance.Location = Location;
				Instance.AlignToNormal(Normal, static_cast<float>(Number(Arguments, TEXT("align_max_angle"), Target.Type->AlignMaxAngle)));
				Target.Info->SetInstanceWorldTransform(Index, Instance.GetInstanceWorldTransform(), true);
				++Aligned;
			}
			Target.Info->PostMoveInstances(Indices, true);
			Target.Info->Refresh(false, true);
			if (Aligned <= 0) { Transaction.Cancel(); Fail(Out, Error, TEXT("surface_projection_failed"), TEXT("No instance had a valid surface projection.")); return false; }
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_align_to_surface"), Before, Aligned, Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_collision_prune"),
		TEXT("Remove foliage instances overlapping blocking world-dynamic geometry inside an optional brush region."),
		{TEXT("foliage_type_path"), TEXT("collision_radius_cm")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), false, Target, Out, Error) || !Target.Info)
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("no_instances"), TEXT("The requested FoliageType has no instances."));
				return false;
			}
			TArray<int32> Candidates;
			FVector Center;
			if (TryVector(Arguments, TEXT("center"), Center)) IndicesInSphere(Target.Info, Center, FMath::Max(0.0, Number(Arguments, TEXT("radius_cm"), 0.0)), Candidates);
			else for (int32 Index = 0; Index < Target.Info->Instances.Num(); ++Index) Candidates.Add(Index);
			TArray<int32> ToRemove;
			const double CollisionRadius = FMath::Max(0.1, Number(Arguments, TEXT("collision_radius_cm"), FMath::Max(1.0f, Target.Type->Radius * 0.5f)));
			for (int32 Index : Candidates) if (HasDynamicCollision(Target.World, Target.Info->Instances[Index].Location, CollisionRadius)) ToRemove.Add(Index);
			if (ToRemove.IsEmpty()) { Fail(Out, Error, TEXT("no_collisions"), TEXT("No foliage instances overlap blocking world-dynamic geometry.")); return false; }
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Collision Prune")));
			Target.Actor->Modify();
			Target.Info->RemoveInstances(ToRemove, true);
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_collision_prune"), Before, ToRemove.Num(), Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_spacing_enforce"),
		TEXT("Enforce a minimum pairwise foliage spacing by deterministically removing later overlapping instances."),
		{TEXT("foliage_type_path"), TEXT("spacing_cm")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFoliageTarget Target;
			if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), false, Target, Out, Error) || !Target.Info)
			{
				if (Error.IsEmpty()) Fail(Out, Error, TEXT("no_instances"), TEXT("The requested FoliageType has no instances."));
				return false;
			}
			const double Spacing = Number(Arguments, TEXT("spacing_cm"), 0.0);
			if (Spacing <= 0.0) { Fail(Out, Error, TEXT("invalid_spacing"), TEXT("spacing_cm must be greater than zero.")); return false; }
			const double SpacingSquared = Spacing * Spacing;
			TSet<int32> Removed;
			for (int32 I = 0; I < Target.Info->Instances.Num(); ++I)
			{
				if (Removed.Contains(I)) continue;
				for (int32 J = I + 1; J < Target.Info->Instances.Num(); ++J)
				{
					if (!Removed.Contains(J) && FVector::DistSquared(Target.Info->Instances[I].Location, Target.Info->Instances[J].Location) < SpacingSquared) Removed.Add(J);
				}
			}
			if (Removed.IsEmpty()) { Fail(Out, Error, TEXT("spacing_already_valid"), TEXT("All instances already satisfy the requested spacing; no write was performed.")); return false; }
			TArray<int32> ToRemove = Removed.Array();
			ToRemove.Sort();
			const int32 Before = Target.Info->Instances.Num();
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Spacing Enforce")));
			Target.Actor->Modify();
			Target.Info->RemoveInstances(ToRemove, true);
			return PersistMutation(Context, Arguments, Target, TEXT("foliage_author_spacing_enforce"), Before, ToRemove.Num(), Out, Summary, Error);
		});

	RegisterTool(Registry, TEXT("foliage_author_blend_brush"),
		TEXT("Paint a weighted mixture of multiple FoliageType assets in one deterministic circular brush."),
		{TEXT("foliage_types"), TEXT("center"), TEXT("count")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			FVector Center;
			const int32 Count = Integer(Arguments, TEXT("count"), 0);
			if (!Arguments->TryGetArrayField(TEXT("foliage_types"), Values) || !Values || Values->IsEmpty() || !TryVector(Arguments, TEXT("center"), Center) || Count <= 0 || Count > 100000)
			{
				Fail(Out, Error, TEXT("invalid_blend_brush"), TEXT("foliage_types, center, and count in [1, 100000] are required."));
				return false;
			}
			struct FWeightedTarget { FFoliageTarget Target; double Weight = 1.0; int32 Before = 0; int32 Added = 0; };
			TArray<FWeightedTarget> Targets;
			double WeightTotal = 0.0;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Path;
				if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty()) { Fail(Out, Error, TEXT("invalid_blend_entry"), TEXT("Every foliage_types entry requires path.")); return false; }
				TSharedRef<FJsonObject> ResolveArgs = MakeShared<FJsonObject>();
				ResolveArgs->SetStringField(TEXT("foliage_type_path"), Path);
				FWeightedTarget Weighted;
				if (!ResolveTarget(Context, ResolveArgs, TEXT("foliage_type_path"), true, Weighted.Target, Out, Error)) return false;
				double RequestedWeight = 1.0;
				Entry->TryGetNumberField(TEXT("weight"), RequestedWeight);
				Weighted.Weight = FMath::Max(0.0, RequestedWeight);
				if (Weighted.Weight <= 0.0) continue;
				Weighted.Before = Weighted.Target.Info->Instances.Num();
				WeightTotal += Weighted.Weight;
				Targets.Add(MoveTemp(Weighted));
			}
			if (Targets.IsEmpty() || WeightTotal <= 0.0) { Fail(Out, Error, TEXT("invalid_weights"), TEXT("At least one blend entry must have a positive weight.")); return false; }
			const double Radius = FMath::Max(0.0, Number(Arguments, TEXT("radius_cm"), 500.0));
			FRandomStream Random(Integer(Arguments, TEXT("seed"), 1337));
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Foliage Blend Brush")));
			for (FWeightedTarget& Weighted : Targets) Weighted.Target.Actor->Modify();
			for (int32 CandidateIndex = 0; CandidateIndex < Count; ++CandidateIndex)
			{
				double Choice = Random.FRandRange(0.0f, static_cast<float>(WeightTotal));
				FWeightedTarget* Selected = &Targets.Last();
				for (FWeightedTarget& Weighted : Targets) { Choice -= Weighted.Weight; if (Choice <= 0.0) { Selected = &Weighted; break; } }
				const double Angle = Random.FRandRange(0.0f, 2.0f * PI);
				const double Distance = FMath::Sqrt(Random.FRand()) * Radius;
				const FVector Candidate = Center + FVector(FMath::Cos(Angle) * Distance, FMath::Sin(Angle) * Distance, 0.0);
				FVector Location, Normal;
				if (!PrepareCandidate(Selected->Target, Arguments, Candidate, Location, Normal)) continue;
				Selected->Target.Info->AddInstance(Selected->Target.Type, MakeInstance(Location, Normal, Selected->Target.Type, Random, Arguments, Selected->Target.Actor->GetRootComponent()));
				++Selected->Added;
			}
			int32 AddedTotal = 0;
			TArray<TSharedPtr<FJsonValue>> TypeReceipts;
			for (FWeightedTarget& Weighted : Targets)
			{
				AddedTotal += Weighted.Added;
				Weighted.Target.Info->Refresh(false, true);
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("foliage_type_path"), Weighted.Target.TypePath);
				Row->SetNumberField(TEXT("instance_count_before"), Weighted.Before);
				Row->SetNumberField(TEXT("instance_count_after"), Weighted.Target.Info->Instances.Num());
				Row->SetNumberField(TEXT("added"), Weighted.Added);
				TypeReceipts.Add(MakeShared<FJsonValueObject>(Row));
			}
			if (AddedTotal <= 0) { Transaction.Cancel(); Fail(Out, Error, TEXT("no_valid_placements"), TEXT("All mixed-brush candidates were rejected by placement filters.")); return false; }
			TArray<UObject*> MutationTargets;
			for (FWeightedTarget& Weighted : Targets)
			{
				MutationTargets.Add(Weighted.Target.Actor);
				MutationTargets.Add(Weighted.Target.Info->GetComponent());
			}
			if (!SaveWorld(Context, Targets[0].Target.World, MutationTargets, Boolean(Arguments, TEXT("save"), true), Out, Error)) return false;
			Out->SetArrayField(TEXT("foliage_type_readbacks"), TypeReceipts);
			bool bPersistenceVerified = false;
			Out->TryGetBoolField(TEXT("persistence_verified"), bPersistenceVerified);
			CompleteReceipt(Out, TEXT("foliage_author_blend_brush"), TEXT("mixed_foliage_types"), 0, AddedTotal, AddedTotal, bPersistenceVerified, Targets[0].Target.Info, Targets[0].Target.Type);
			Summary = FString::Printf(TEXT("Painted %d mixed foliage instances across %d FoliageTypes."), AddedTotal, Targets.Num());
			return true;
		});

	auto RegisterFoliageToISM = [&Registry](const FString& ToolName, bool bHierarchical)
	{
		RegisterTool(Registry, ToolName,
			bHierarchical
				? TEXT("Convert selected/all foliage instances into a native HISM component while preserving world transforms.")
				: TEXT("Convert selected/all foliage instances into a native ISM component while preserving world transforms."),
			{TEXT("foliage_type_path")},
			[ToolName, bHierarchical](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
			{
				FFoliageTarget Target;
				if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), false, Target, Out, Error) || !Target.Info)
				{
					if (Error.IsEmpty()) Fail(Out, Error, TEXT("no_instances"), TEXT("The requested FoliageType has no instances."));
					return false;
				}
				UFoliageType_InstancedStaticMesh* MeshType = Cast<UFoliageType_InstancedStaticMesh>(Target.Type);
				if (!MeshType || !MeshType->GetStaticMesh())
				{
					Fail(Out, Error, TEXT("unsupported_foliage_type"), TEXT("Conversion requires UFoliageType_InstancedStaticMesh with a valid StaticMesh."));
					return false;
				}
				TArray<int32> Indices;
				FVector Center;
				if (TryVector(Arguments, TEXT("center"), Center)) IndicesInSphere(Target.Info, Center, FMath::Max(0.0, Number(Arguments, TEXT("radius_cm"), 0.0)), Indices);
				else for (int32 Index = 0; Index < Target.Info->Instances.Num(); ++Index) Indices.Add(Index);
				if (Indices.IsEmpty()) { Fail(Out, Error, TEXT("no_matching_instances"), TEXT("No foliage instances matched the conversion region.")); return false; }
				FString RequestedName;
				Arguments->TryGetStringField(TEXT("actor"), RequestedName);
				if (!RequestedName.IsEmpty() && FindActor(Target.World, RequestedName))
				{
					Fail(Out, Error, TEXT("actor_name_collision"), TEXT("An actor with the requested name or label already exists."));
					return false;
				}
				FAtomicFoliageTransaction Transaction(FText::FromString(bHierarchical ? TEXT("SOMOLMCP Foliage To HISM") : TEXT("SOMOLMCP Foliage To ISM")));
				FActorSpawnParameters Spawn;
				Spawn.Name = MakeUniqueObjectName(Target.World->GetCurrentLevel(), AActor::StaticClass(), RequestedName.IsEmpty() ? FName(TEXT("SOMOL_FoliageInstances")) : FName(*RequestedName));
				Spawn.ObjectFlags |= RF_Transactional;
				Target.World->GetCurrentLevel()->Modify();
				AActor* Owner = Target.World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
				if (!Owner) { Fail(Out, Error, TEXT("spawn_failed"), TEXT("Failed to spawn the conversion owner actor.")); return false; }
				Owner->Modify();
				Owner->SetActorLabel(RequestedName.IsEmpty() ? Spawn.Name.ToString() : RequestedName);
				USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"), RF_Transactional);
				Owner->SetRootComponent(Root);
				Owner->AddInstanceComponent(Root);
				Root->RegisterComponent();
				UInstancedStaticMeshComponent* Component = bHierarchical
					? static_cast<UInstancedStaticMeshComponent*>(NewObject<UHierarchicalInstancedStaticMeshComponent>(Owner, TEXT("FoliageHISM"), RF_Transactional))
					: NewObject<UInstancedStaticMeshComponent>(Owner, TEXT("FoliageISM"), RF_Transactional);
				if (!Component) { Target.World->DestroyActor(Owner); Fail(Out, Error, TEXT("component_create_failed"), TEXT("Failed to create the ISM/HISM component.")); return false; }
				Component->SetupAttachment(Root);
				Component->SetStaticMesh(MeshType->GetStaticMesh());
				Owner->AddInstanceComponent(Component);
				Component->RegisterComponent();
				for (int32 Index : Indices) Component->AddInstance(Target.Info->Instances[Index].GetInstanceWorldTransform(), true);
				if (Component->GetInstanceCount() != Indices.Num())
				{
					Target.World->DestroyActor(Owner);
					Fail(Out, Error, TEXT("component_readback_mismatch"), TEXT("ISM/HISM instance count did not match the copied foliage count."));
					return false;
				}
				const int32 Before = Target.Info->Instances.Num();
				if (Boolean(Arguments, TEXT("remove_source"), true)) Target.Info->RemoveInstances(Indices, true);
				TArray<UObject*> MutationTargets{Target.Actor, Target.Info->GetComponent(), Owner, Root, Component};
				if (!SaveWorld(Context, Target.World, MutationTargets, Boolean(Arguments, TEXT("save"), true), Out, Error)) return false;
				bool bPersistenceVerified = false;
				Out->TryGetBoolField(TEXT("persistence_verified"), bPersistenceVerified);
				CompleteReceipt(Out, ToolName, Target.TypePath, Before, Target.Info->Instances.Num(), Indices.Num(), bPersistenceVerified, Target.Info, Target.Type);
				Out->SetStringField(TEXT("created_actor"), Owner->GetPathName());
				Out->SetStringField(TEXT("created_component"), Component->GetPathName());
				Out->SetNumberField(TEXT("component_instance_count"), Component->GetInstanceCount());
				Summary = FString::Printf(TEXT("Converted %d foliage instances to %s '%s'."), Indices.Num(), bHierarchical ? TEXT("HISM") : TEXT("ISM"), *Owner->GetActorLabel());
				return true;
			});
	};
	RegisterFoliageToISM(TEXT("foliage_author_foliage_to_hism"), true);
	RegisterFoliageToISM(TEXT("foliage_author_foliage_to_ism"), false);

	auto RegisterISMToFoliage = [&Registry](const FString& ToolName, bool bRequireHISM)
	{
		RegisterTool(Registry, ToolName,
			bRequireHISM
				? TEXT("Convert a named HISM component into native foliage instances with mesh and count validation.")
				: TEXT("Convert a named non-hierarchical ISM component into native foliage instances with mesh and count validation."),
			{TEXT("foliage_type_path"), TEXT("actor")},
			[ToolName, bRequireHISM](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
			{
				FFoliageTarget Target;
				if (!ResolveTarget(Context, Arguments, TEXT("foliage_type_path"), true, Target, Out, Error)) return false;
				UFoliageType_InstancedStaticMesh* MeshType = Cast<UFoliageType_InstancedStaticMesh>(Target.Type);
				if (!MeshType || !MeshType->GetStaticMesh()) { Fail(Out, Error, TEXT("unsupported_foliage_type"), TEXT("Conversion requires a mesh-backed FoliageType.")); return false; }
				FString ActorId, ComponentId;
				Arguments->TryGetStringField(TEXT("actor"), ActorId);
				Arguments->TryGetStringField(TEXT("component"), ComponentId);
				AActor* Owner = FindActor(Target.World, ActorId);
				if (!Owner) { Fail(Out, Error, TEXT("actor_not_found"), TEXT("The source actor was not found by name, label, or path.")); return false; }
				UInstancedStaticMeshComponent* Component = FindISMComponent(Owner, ComponentId, bRequireHISM, !bRequireHISM);
				if (!Component) { Fail(Out, Error, TEXT("component_not_found"), bRequireHISM ? TEXT("No matching HISM component was found.") : TEXT("No matching non-hierarchical ISM component was found.")); return false; }
				if (Component->GetStaticMesh() != MeshType->GetStaticMesh())
				{
					Fail(Out, Error, TEXT("mesh_mismatch"), TEXT("Source component mesh does not match the target FoliageType mesh."));
					return false;
				}
				const int32 SourceCount = Component->GetInstanceCount();
				if (SourceCount <= 0) { Fail(Out, Error, TEXT("empty_component"), TEXT("The source ISM/HISM component contains no instances.")); return false; }
				TArray<FTransform> Transforms;
				Transforms.Reserve(SourceCount);
				for (int32 Index = 0; Index < SourceCount; ++Index)
				{
					FTransform Transform;
					if (!Component->GetInstanceTransform(Index, Transform, true)) { Fail(Out, Error, TEXT("transform_readback_failed"), FString::Printf(TEXT("Failed to read source transform %d."), Index)); return false; }
					Transforms.Add(Transform);
				}
				const int32 Before = Target.Info->Instances.Num();
				FAtomicFoliageTransaction Transaction(FText::FromString(bRequireHISM ? TEXT("SOMOLMCP HISM To Foliage") : TEXT("SOMOLMCP ISM To Foliage")));
				Target.Actor->Modify();
				Owner->Modify();
				Component->Modify();
				for (const FTransform& Transform : Transforms)
				{
					FFoliageInstance Instance;
					Instance.SetInstanceWorldTransform(Transform);
					Instance.BaseComponent = Target.Actor->GetRootComponent();
					Target.Info->AddInstance(Target.Type, Instance);
				}
				Target.Info->Refresh(false, true);
				if (Target.Info->Instances.Num() - Before != SourceCount)
				{
					Fail(Out, Error, TEXT("foliage_readback_mismatch"), TEXT("The foliage instance count did not increase by the source component count."));
					return false;
				}
				if (Boolean(Arguments, TEXT("remove_source"), true))
				{
					Component->ClearInstances();
					if (Boolean(Arguments, TEXT("destroy_component"), false)) Component->DestroyComponent();
				}
				TArray<UObject*> MutationTargets{Target.Actor, Target.Info->GetComponent(), Owner, Component};
				if (!SaveWorld(Context, Target.World, MutationTargets, Boolean(Arguments, TEXT("save"), true), Out, Error)) return false;
				bool bPersistenceVerified = false;
				Out->TryGetBoolField(TEXT("persistence_verified"), bPersistenceVerified);
				CompleteReceipt(Out, ToolName, Target.TypePath, Before, Target.Info->Instances.Num(), SourceCount, bPersistenceVerified, Target.Info, Target.Type);
				Out->SetStringField(TEXT("source_actor"), Owner->GetPathName());
				Out->SetStringField(TEXT("source_component"), ComponentId.IsEmpty() ? TEXT("auto") : ComponentId);
				Out->SetNumberField(TEXT("source_instance_count"), SourceCount);
				Summary = FString::Printf(TEXT("Converted %d %s instances to foliage type %s."), SourceCount, bRequireHISM ? TEXT("HISM") : TEXT("ISM"), *Target.TypePath);
				return true;
			});
	};
	RegisterISMToFoliage(TEXT("foliage_author_hism_to_foliage"), true);
	RegisterISMToFoliage(TEXT("foliage_author_ism_to_foliage"), false);

	RegisterTool(Registry, TEXT("foliage_author_type_get_properties"),
		TEXT("Read placement, scale, collision, culling, shadow, Nanite-distance, and scalability properties from a FoliageType."),
		{TEXT("foliage_type_path")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Path;
			Arguments->TryGetStringField(TEXT("foliage_type_path"), Path);
			UFoliageType* Type = Cast<UFoliageType>(Context.Services.LoadAsset(Path, Error));
			if (!Type) { Fail(Out, Error, TEXT("invalid_foliage_type"), Error.IsEmpty() ? TEXT("The asset is not a UFoliageType.") : Error); return false; }
			int32 Count = 0;
			FString WorldError;
			if (UWorld* World = Context.Services.GetEditorWorld(WorldError))
			{
				for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It) if (const FFoliageInfo* Info = It->FindInfo(Type)) Count += Info->Instances.Num();
			}
			Out->SetObjectField(TEXT("properties"), FoliageTypePropertiesJson(Type));
			CompleteReceipt(Out, TEXT("foliage_author_type_get_properties"), Path, Count, Count, 0, true, nullptr, Type);
			Summary = FString::Printf(TEXT("Read FoliageType properties for %s; current-world instances=%d."), *Path, Count);
			return true;
		});

	RegisterTool(Registry, TEXT("foliage_author_type_set_properties"),
		TEXT("Write core FoliageType paint and placement properties, notify foliage actors, save, and read back values."),
		{TEXT("foliage_type_path"), TEXT("properties")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Path;
			Arguments->TryGetStringField(TEXT("foliage_type_path"), Path);
			UFoliageType* Type = Cast<UFoliageType>(Context.Services.LoadAsset(Path, Error));
			const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
			if (!Type || !Arguments->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr || !PropertiesPtr->IsValid())
			{
				Fail(Out, Error, TEXT("invalid_request"), Type ? TEXT("properties object is required.") : (Error.IsEmpty() ? TEXT("The asset is not a UFoliageType.") : Error));
				return false;
			}
			const TSharedRef<FJsonObject> Properties = PropertiesPtr->ToSharedRef();
			int32 Changed = 0;
			auto SetNumber = [&Properties, &Changed](const TCHAR* Name, float& Field, float Min, float Max) { double V; if (Properties->TryGetNumberField(Name, V)) { Field = FMath::Clamp(static_cast<float>(V), Min, Max); ++Changed; } };
			auto SetBool = [&Properties, &Changed](const TCHAR* Name, TFunctionRef<void(bool)> Setter) { bool V; if (Properties->TryGetBoolField(Name, V)) { Setter(V); ++Changed; } };
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP FoliageType Set Properties")));
			Type->Modify();
			SetNumber(TEXT("density"), Type->Density, 0.0f, 10000.0f);
			SetNumber(TEXT("density_adjustment_factor"), Type->DensityAdjustmentFactor, 0.0f, 1000.0f);
			SetNumber(TEXT("radius"), Type->Radius, 0.0f, 100000.0f);
			SetBool(TEXT("single_instance_override_radius"), [Type](bool V) { Type->bSingleInstanceModeOverrideRadius = V; });
			SetNumber(TEXT("single_instance_radius"), Type->SingleInstanceModeRadius, 0.0f, 100000.0f);
			FVector ScaleMin, ScaleMax;
			if (TryVector(Properties, TEXT("scale_min"), ScaleMin)) { Type->ScaleX.Min = FMath::Max(0.001, ScaleMin.X); Type->ScaleY.Min = FMath::Max(0.001, ScaleMin.Y); Type->ScaleZ.Min = FMath::Max(0.001, ScaleMin.Z); ++Changed; }
			if (TryVector(Properties, TEXT("scale_max"), ScaleMax)) { Type->ScaleX.Max = FMath::Max(Type->ScaleX.Min, static_cast<float>(ScaleMax.X)); Type->ScaleY.Max = FMath::Max(Type->ScaleY.Min, static_cast<float>(ScaleMax.Y)); Type->ScaleZ.Max = FMath::Max(Type->ScaleZ.Min, static_cast<float>(ScaleMax.Z)); ++Changed; }
			SetNumber(TEXT("z_offset_min"), Type->ZOffset.Min, -100000.0f, 100000.0f);
			SetNumber(TEXT("z_offset_max"), Type->ZOffset.Max, Type->ZOffset.Min, 100000.0f);
			SetBool(TEXT("align_to_normal"), [Type](bool V) { Type->AlignToNormal = V; });
			SetNumber(TEXT("align_max_angle"), Type->AlignMaxAngle, 0.0f, 359.0f);
			SetBool(TEXT("random_yaw"), [Type](bool V) { Type->RandomYaw = V; });
			SetNumber(TEXT("random_pitch_angle"), Type->RandomPitchAngle, 0.0f, 359.0f);
			SetNumber(TEXT("slope_min"), Type->GroundSlopeAngle.Min, 0.0f, 359.0f);
			SetNumber(TEXT("slope_max"), Type->GroundSlopeAngle.Max, Type->GroundSlopeAngle.Min, 359.0f);
			SetNumber(TEXT("height_min"), Type->Height.Min, -HALF_WORLD_MAX, HALF_WORLD_MAX);
			SetNumber(TEXT("height_max"), Type->Height.Max, Type->Height.Min, HALF_WORLD_MAX);
			SetBool(TEXT("collision_with_world"), [Type](bool V) { Type->CollisionWithWorld = V; });
			FVector CollisionScale;
			if (TryVector(Properties, TEXT("collision_scale"), CollisionScale) && CollisionScale.GetMin() > 0.0) { Type->CollisionScale = CollisionScale; ++Changed; }
			SetNumber(TEXT("minimum_layer_weight"), Type->MinimumLayerWeight, 0.0f, 1.0f);
			if (Changed <= 0) { Transaction.Cancel(); Fail(Out, Error, TEXT("no_supported_properties"), TEXT("No supported placement property was supplied; no write was performed.")); return false; }
			Type->UpdateGuid = FGuid::NewGuid();
			FString WorldError;
			NotifyFoliageTypeChanged(Context.Services.GetEditorWorld(WorldError), Type, false);
			if (!SaveType(Context, Type, Boolean(Arguments, TEXT("save"), true), Out, Error)) return false;
			Out->SetObjectField(TEXT("properties_readback"), FoliageTypePropertiesJson(Type));
			bool bPersistenceVerified = false;
			Out->TryGetBoolField(TEXT("persistence_verified"), bPersistenceVerified);
			CompleteReceipt(Out, TEXT("foliage_author_type_set_properties"), Path, 0, 0, Changed, bPersistenceVerified, nullptr, Type);
			Summary = FString::Printf(TEXT("Updated and read back %d FoliageType placement properties for %s."), Changed, *Path);
			return true;
		});

	RegisterTool(Registry, TEXT("foliage_author_type_set_mesh"),
		TEXT("Assign a StaticMesh to a mesh-backed FoliageType, notify placed instances, save, and read back the source mesh."),
		{TEXT("foliage_type_path"), TEXT("mesh_path")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString TypePath, MeshPath;
			Arguments->TryGetStringField(TEXT("foliage_type_path"), TypePath);
			Arguments->TryGetStringField(TEXT("mesh_path"), MeshPath);
			UFoliageType_InstancedStaticMesh* Type = Cast<UFoliageType_InstancedStaticMesh>(Context.Services.LoadAsset(TypePath, Error));
			UStaticMesh* Mesh = Cast<UStaticMesh>(Context.Services.LoadAsset(MeshPath, Error));
			if (!Type || !Mesh) { Fail(Out, Error, TEXT("invalid_mesh_assignment"), Error.IsEmpty() ? TEXT("A mesh-backed FoliageType and StaticMesh are required.") : Error); return false; }
			if (Type->GetStaticMesh() == Mesh) { Fail(Out, Error, TEXT("mesh_already_assigned"), TEXT("The requested StaticMesh is already assigned; no write was performed.")); return false; }
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP FoliageType Set Mesh")));
			Type->Modify();
			Type->SetStaticMesh(Mesh);
			Type->UpdateGuid = FGuid::NewGuid();
			FString WorldError;
			NotifyFoliageTypeChanged(Context.Services.GetEditorWorld(WorldError), Type, true);
			if (!SaveType(Context, Type, Boolean(Arguments, TEXT("save"), true), Out, Error)) return false;
			Out->SetStringField(TEXT("mesh_path_readback"), Type->GetStaticMesh() ? Type->GetStaticMesh()->GetPathName() : FString());
			bool bPersistenceVerified = false;
			Out->TryGetBoolField(TEXT("persistence_verified"), bPersistenceVerified);
			CompleteReceipt(Out, TEXT("foliage_author_type_set_mesh"), TypePath, 0, 0, 1, bPersistenceVerified, nullptr, Type);
			Summary = FString::Printf(TEXT("Assigned mesh %s to FoliageType %s."), *MeshPath, *TypePath);
			return true;
		});

	RegisterTool(Registry, TEXT("foliage_author_type_set_reapply_settings"),
		TEXT("Write the complete FoliageType reapply flag set, save, and return flag readback."),
		{TEXT("foliage_type_path"), TEXT("properties")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Path;
			Arguments->TryGetStringField(TEXT("foliage_type_path"), Path);
			UFoliageType* Type = Cast<UFoliageType>(Context.Services.LoadAsset(Path, Error));
			const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
			if (!Type || !Arguments->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr || !PropertiesPtr->IsValid()) { Fail(Out, Error, TEXT("invalid_request"), TEXT("A FoliageType and properties object are required.")); return false; }
			const TSharedRef<FJsonObject> P = PropertiesPtr->ToSharedRef();
			int32 Changed = 0;
			auto SetFlag = [&P, &Changed](const TCHAR* Name, TFunctionRef<void(bool)> Setter) { bool V; if (P->TryGetBoolField(Name, V)) { Setter(V); ++Changed; } };
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP FoliageType Reapply Settings")));
			Type->Modify();
			SetFlag(TEXT("reapply_density"), [Type](bool V) { Type->ReapplyDensity = V; });
			SetFlag(TEXT("reapply_radius"), [Type](bool V) { Type->ReapplyRadius = V; });
			SetFlag(TEXT("reapply_align_to_normal"), [Type](bool V) { Type->ReapplyAlignToNormal = V; });
			SetFlag(TEXT("reapply_random_yaw"), [Type](bool V) { Type->ReapplyRandomYaw = V; });
			SetFlag(TEXT("reapply_scaling"), [Type](bool V) { Type->ReapplyScaling = V; });
			SetFlag(TEXT("reapply_scale_x"), [Type](bool V) { Type->ReapplyScaleX = V; });
			SetFlag(TEXT("reapply_scale_y"), [Type](bool V) { Type->ReapplyScaleY = V; });
			SetFlag(TEXT("reapply_scale_z"), [Type](bool V) { Type->ReapplyScaleZ = V; });
			SetFlag(TEXT("reapply_random_pitch"), [Type](bool V) { Type->ReapplyRandomPitchAngle = V; });
			SetFlag(TEXT("reapply_ground_slope"), [Type](bool V) { Type->ReapplyGroundSlope = V; });
			SetFlag(TEXT("reapply_height"), [Type](bool V) { Type->ReapplyHeight = V; });
			SetFlag(TEXT("reapply_landscape_layers"), [Type](bool V) { Type->ReapplyLandscapeLayers = V; });
			SetFlag(TEXT("reapply_z_offset"), [Type](bool V) { Type->ReapplyZOffset = V; });
			SetFlag(TEXT("reapply_collision"), [Type](bool V) { Type->ReapplyCollisionWithWorld = V; });
			if (Changed <= 0) { Transaction.Cancel(); Fail(Out, Error, TEXT("no_reapply_flags"), TEXT("No supported reapply flag was supplied.")); return false; }
			Type->UpdateGuid = FGuid::NewGuid();
			if (!SaveType(Context, Type, Boolean(Arguments, TEXT("save"), true), Out, Error)) return false;
			TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
			Readback->SetBoolField(TEXT("reapply_density"), !!Type->ReapplyDensity);
			Readback->SetBoolField(TEXT("reapply_radius"), !!Type->ReapplyRadius);
			Readback->SetBoolField(TEXT("reapply_align_to_normal"), !!Type->ReapplyAlignToNormal);
			Readback->SetBoolField(TEXT("reapply_random_yaw"), !!Type->ReapplyRandomYaw);
			Readback->SetBoolField(TEXT("reapply_scaling"), !!Type->ReapplyScaling);
			Readback->SetBoolField(TEXT("reapply_collision"), !!Type->ReapplyCollisionWithWorld);
			Out->SetObjectField(TEXT("properties_readback"), Readback);
			bool bPersistenceVerified = false;
			Out->TryGetBoolField(TEXT("persistence_verified"), bPersistenceVerified);
			CompleteReceipt(Out, TEXT("foliage_author_type_set_reapply_settings"), Path, 0, 0, Changed, bPersistenceVerified, nullptr, Type);
			Summary = FString::Printf(TEXT("Updated %d FoliageType reapply flags for %s."), Changed, *Path);
			return true;
		});

	RegisterTool(Registry, TEXT("foliage_author_type_set_render_properties"),
		TEXT("Write FoliageType culling, mobility, lighting, shadow, ray tracing, WPO, custom-depth, Nanite-distance, scalability, RVT, and HLOD properties."),
		{TEXT("foliage_type_path"), TEXT("properties")},
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Path;
			Arguments->TryGetStringField(TEXT("foliage_type_path"), Path);
			UFoliageType* Type = Cast<UFoliageType>(Context.Services.LoadAsset(Path, Error));
			const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
			if (!Type || !Arguments->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr || !PropertiesPtr->IsValid()) { Fail(Out, Error, TEXT("invalid_request"), TEXT("A FoliageType and properties object are required.")); return false; }
			const TSharedRef<FJsonObject> P = PropertiesPtr->ToSharedRef();
			int32 Changed = 0;
			auto SetFlag = [&P, &Changed](const TCHAR* Name, TFunctionRef<void(bool)> Setter) { bool V; if (P->TryGetBoolField(Name, V)) { Setter(V); ++Changed; } };
			FAtomicFoliageTransaction Transaction(FText::FromString(TEXT("SOMOLMCP FoliageType Render Properties")));
			Type->Modify();
			double V = 0.0;
			if (P->TryGetNumberField(TEXT("cull_start"), V)) { Type->CullDistance.Min = FMath::Max(0, FMath::RoundToInt(V)); ++Changed; }
			if (P->TryGetNumberField(TEXT("cull_end"), V)) { Type->CullDistance.Max = FMath::Max(Type->CullDistance.Min, FMath::RoundToInt(V)); ++Changed; }
			FString Mobility;
			if (P->TryGetStringField(TEXT("mobility"), Mobility))
			{
				if (Mobility == TEXT("static")) Type->Mobility = EComponentMobility::Static;
				else if (Mobility == TEXT("stationary")) Type->Mobility = EComponentMobility::Stationary;
				else if (Mobility == TEXT("movable")) Type->Mobility = EComponentMobility::Movable;
				else { Transaction.Cancel(); Fail(Out, Error, TEXT("invalid_mobility"), TEXT("mobility must be static, stationary, or movable.")); return false; }
				++Changed;
			}
			SetFlag(TEXT("cast_shadow"), [Type](bool B) { Type->CastShadow = B; });
			SetFlag(TEXT("cast_dynamic_shadow"), [Type](bool B) { Type->bCastDynamicShadow = B; });
			SetFlag(TEXT("cast_static_shadow"), [Type](bool B) { Type->bCastStaticShadow = B; });
			SetFlag(TEXT("cast_contact_shadow"), [Type](bool B) { Type->bCastContactShadow = B; });
			SetFlag(TEXT("affect_dynamic_indirect_lighting"), [Type](bool B) { Type->bAffectDynamicIndirectLighting = B; });
			SetFlag(TEXT("affect_distance_field_lighting"), [Type](bool B) { Type->bAffectDistanceFieldLighting = B; });
			SetFlag(TEXT("visible_in_ray_tracing"), [Type](bool B) { Type->bVisibleInRayTracing = B; });
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
			SetFlag(TEXT("visible_in_reflections"), [Type](bool B) { Type->bVisibleInReflections = B; });
#endif
			SetFlag(TEXT("evaluate_world_position_offset"), [Type](bool B) { Type->bEvaluateWorldPositionOffset = B; });
			SetFlag(TEXT("render_custom_depth"), [Type](bool B) { Type->bRenderCustomDepth = B; });
			SetFlag(TEXT("enable_density_scaling"), [Type](bool B) { Type->bEnableDensityScaling = B; });
			SetFlag(TEXT("enable_cull_distance_scaling"), [Type](bool B) { Type->bEnableCullDistanceScaling = B; });
			if (P->TryGetNumberField(TEXT("world_position_offset_disable_distance"), V)) { Type->WorldPositionOffsetDisableDistance = FMath::Max(0, FMath::RoundToInt(V)); ++Changed; }
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
			if (P->TryGetNumberField(TEXT("nanite_pixel_programmable_distance"), V)) { Type->NanitePixelProgrammableDistance = FMath::Max(0.0, V); ++Changed; }
#endif
			if (P->TryGetNumberField(TEXT("custom_depth_stencil_value"), V)) { Type->CustomDepthStencilValue = FMath::Clamp(FMath::RoundToInt(V), 0, 255); ++Changed; }
			if (P->TryGetNumberField(TEXT("virtual_texture_cull_mips"), V)) { Type->VirtualTextureCullMips = FMath::Clamp(FMath::RoundToInt(V), 0, 7); ++Changed; }
#if WITH_EDITORONLY_DATA
			SetFlag(TEXT("include_in_hlod"), [Type](bool B) { Type->bIncludeInHLOD = B; });
#endif
			if (Changed <= 0) { Transaction.Cancel(); Fail(Out, Error, TEXT("no_render_properties"), TEXT("No supported render property was supplied.")); return false; }
			Type->UpdateGuid = FGuid::NewGuid();
			FString WorldError;
			NotifyFoliageTypeChanged(Context.Services.GetEditorWorld(WorldError), Type, false);
			if (!SaveType(Context, Type, Boolean(Arguments, TEXT("save"), true), Out, Error)) return false;
			Out->SetObjectField(TEXT("properties_readback"), FoliageTypePropertiesJson(Type));
			bool bPersistenceVerified = false;
			Out->TryGetBoolField(TEXT("persistence_verified"), bPersistenceVerified);
			CompleteReceipt(Out, TEXT("foliage_author_type_set_render_properties"), Path, 0, 0, Changed, bPersistenceVerified, nullptr, Type);
			Summary = FString::Printf(TEXT("Updated and read back %d FoliageType render properties for %s."), Changed, *Path);
			return true;
		});
}
}
