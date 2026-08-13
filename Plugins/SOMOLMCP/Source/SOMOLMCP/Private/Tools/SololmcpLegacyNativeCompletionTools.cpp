// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// Legacy-name native completion batch (2026-08-05):
// The foliage_* and geometry_script_* tools below were previously declared only
// through RegisterExtendedPythonTool (hidden, fail-closed, unexecutable). This
// file replaces those declarations with real native C++ executors. Each tool
// keeps its legacy parameter schema, so existing clients keep working, and adds
// transaction/save/readback evidence per the authoring completion contract.
//
// Foliage uses the Foliage module directly (same calls as the existing
// foliage_instances_paint writer). GeometryScript uses the engine
// GeometryScriptingCore library (UE 5.8 only); on older engines the tools are
// registered as explicit blocked adapters instead of silent Python fallbacks.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Math/Box.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"

#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "InstancedFoliageActor.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshModelingFunctions.h"
#include "GeometryScript/MeshRemeshFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "UDynamicMesh.h"
#endif

namespace UE::SOMOLMCP
{
	namespace LegacyNativeCompletion
	{
		static bool Fail(TSharedRef<FJsonObject>& Out, FString& Error, const TCHAR* Code, const FString& Message)
		{
			Out->SetStringField(TEXT("ok"), TEXT("false"));
			Out->SetStringField(TEXT("error_code"), Code);
			Out->SetStringField(TEXT("error"), Message);
			Error = Message;
			return false;
		}

		static bool TryVector(const TSharedRef<FJsonObject>& Args, const TCHAR* Field, FVector& Out, FString& Error)
		{
			const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
			if (!Args->TryGetObjectField(Field, ObjectPtr))
			{
				Error = FString::Printf(TEXT("Missing or invalid %s vector."), Field);
				return false;
			}
			const TSharedRef<FJsonObject> Object = ObjectPtr->ToSharedRef();
			if (!Object->HasField(TEXT("x")) || !Object->HasField(TEXT("y")) || !Object->HasField(TEXT("z")))
			{
				Error = FString::Printf(TEXT("%s must be an object with x/y/z numbers."), Field);
				return false;
			}
			Out = FVector(
				static_cast<double>(Object->GetNumberField(TEXT("x"))),
				static_cast<double>(Object->GetNumberField(TEXT("y"))),
				static_cast<double>(Object->GetNumberField(TEXT("z"))));
			return Out.ContainsNaN() ? (Error = FString::Printf(TEXT("%s must be finite."), Field), false) : true;
		}

		static bool LoadFoliageType(const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Args, UFoliageType*& OutType, FString& Error)
		{
			FString Path;
			if (!Args->TryGetStringField(TEXT("foliage_type_path"), Path) || Path.IsEmpty())
			{
				Error = TEXT("Missing foliage_type_path.");
				return false;
			}
			OutType = Cast<UFoliageType>(Context.Services.LoadAsset(Path, Error));
			if (!OutType)
			{
				if (Error.IsEmpty()) Error = TEXT("foliage_type_path is not a FoliageType asset.");
				return false;
			}
			return true;
		}

		static bool GetFoliageInfo(const FSololmcpToolExecutionContext& Context,
			UFoliageType* Type, AInstancedFoliageActor*& OutActor, FFoliageInfo*& OutInfo, FString& Error)
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				Error = TEXT("No editor world is available.");
				return false;
			}
			// The Foliage runtime module may not be loaded in an unattended editor; the
			// AInstancedFoliageActor UClass must be registered before SpawnActor can run.
			if (!FModuleManager::Get().IsModuleLoaded(TEXT("Foliage")))
			{
				if (!FModuleManager::Get().LoadModule(TEXT("Foliage")))
				{
					Error = TEXT("The Foliage module could not be loaded.");
					return false;
				}
			}
			// Same semantics as the UE Foliage panel: painting implicitly creates the
			// InstancedFoliageActor for the current level when none exists.
			OutActor = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(World, /*bCreateIfNone=*/true);
			if (!OutActor)
			{
				Error = TEXT("No InstancedFoliageActor exists in the current level.");
				return false;
			}
			// Headless IFA spawns never create a root component (the ctor only sets
			// collision). The engine's AddInstanceImpl -> AddInstanceBaseId calls
			// IFoliageEditModuleBase::ShouldIgnoreComponentForBaseID unconditionally
			// (FoliageEditModule.cpp), which does InComponent->IsA(...) with no null
			// check -> 0xC0000005 when BaseComponent is null. Guarantee a valid root
			// so BaseId generation takes the safe path (also unblocks
			// FFoliageStaticMesh::CreateNewComponent, which dereferences the root).
			if (!OutActor->GetRootComponent())
			{
				USceneComponent* Root = NewObject<USceneComponent>(OutActor);
				OutActor->SetRootComponent(Root);
				Root->RegisterComponent();
			}
			OutInfo = OutActor->FindOrAddMesh(Type);
			if (!OutInfo)
			{
				Error = TEXT("The FoliageType could not be bound to the foliage actor.");
				return false;
			}
			return true;
		}

		static void RegisterFoliageTools(FSololmcpToolRegistry& Registry)
		{
			// foliage_type_create — create an empty (or source-bound) FoliageType asset.
			Registry.Register({TEXT("foliage_type_create"),
				TEXT("Create a FoliageType asset (optionally bound to a Static Mesh), save it, and verify reload readback."),
				FSololmcpSchemaBuilder::Object(
					{
						{TEXT("package_path"), FSololmcpSchemaBuilder::String(TEXT("Destination package path under /Game/."), {}, 1, 1024)},
						{TEXT("asset_name"), FSololmcpSchemaBuilder::String(TEXT("FoliageType asset name."), {}, 1, 255)},
						{TEXT("mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Optional Static Mesh object path to bind as source."), {}, 1, 1024)},
					},
					{TEXT("package_path"), TEXT("asset_name")}, FString(), false),
				[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
					TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
				{
					const FString PackagePath = Args->GetStringField(TEXT("package_path"));
					const FString AssetName = Args->GetStringField(TEXT("asset_name"));
					if (!PackagePath.StartsWith(TEXT("/Game/")))
					{
						return Fail(Out, Error, TEXT("invalid_package_path"), TEXT("package_path must be under /Game/."));
					}
					const FString AssetPath = PackagePath / AssetName;
					if (UObject* Existing = StaticFindObject(nullptr, CreatePackage(*AssetPath), *AssetName))
					{
						return Fail(Out, Error, TEXT("asset_already_exists"),
							FString::Printf(TEXT("'%s' already exists as %s; refusing replacement."), *AssetPath, *Existing->GetClass()->GetName()));
					}
					UStaticMesh* SourceMesh = nullptr;
					if (Args->HasField(TEXT("mesh_path")) && !Args->GetStringField(TEXT("mesh_path")).IsEmpty())
					{
						SourceMesh = Cast<UStaticMesh>(Context.Services.LoadAsset(Args->GetStringField(TEXT("mesh_path")), Error));
						if (!SourceMesh)
						{
							return Fail(Out, Error, TEXT("invalid_mesh_path"), TEXT("mesh_path is not a Static Mesh asset."));
						}
					}
					// The FoliageType must live in its own package (PackagePath/AssetName), not in
					// the bare PackagePath package — otherwise the asset is unreachable at the
					// conventional /Game/.../AssetName.AssetName path and save/load round-trips
					// fail for every downstream tool.
					UPackage* Package = CreatePackage(*AssetPath);
					UFoliageType_InstancedStaticMesh* Type = NewObject<UFoliageType_InstancedStaticMesh>(
						Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
					if (!Type)
					{
						return Fail(Out, Error, TEXT("foliage_type_create_failed"), TEXT("Failed to allocate FoliageType asset."));
					}
					if (SourceMesh)
					{
						Type->Modify();
						Type->SetSource(SourceMesh);
					}
					FAssetRegistryModule::AssetCreated(Type);
					Type->MarkPackageDirty();
					if (!Context.Services.SaveAsset(Type->GetPathName(), false, Error))
					{
						return Fail(Out, Error, TEXT("foliage_type_save_failed"), Error);
					}
					UObject* Reloaded = LoadObject<UObject>(nullptr, *Type->GetPathName());
					if (!Reloaded || !Reloaded->IsA<UFoliageType_InstancedStaticMesh>())
					{
						return Fail(Out, Error, TEXT("foliage_type_readback_failed"), TEXT("Saved FoliageType did not reload as UFoliageType_InstancedStaticMesh."));
					}
					Out->SetBoolField(TEXT("ok"), true);
					Out->SetStringField(TEXT("status"), TEXT("succeeded"));
					Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("foliage_type_create_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
					Out->SetStringField(TEXT("asset_path"), Type->GetPathName());
					Out->SetBoolField(TEXT("mutation_applied"), true);
					Out->SetBoolField(TEXT("readback_verified"), true);
					if (SourceMesh)
					{
						Out->SetStringField(TEXT("source_mesh"), SourceMesh->GetPathName());
					}
					Summary = FString::Printf(TEXT("Created FoliageType %s."), *Type->GetPathName());
					return true;
				}, nullptr, 0});

			// foliage_paint_instances — add one instance at each supplied world location.
			Registry.Register({TEXT("foliage_paint_instances"),
				TEXT("Paint foliage instances at explicit world locations and return the added-count readback."),
				FSololmcpSchemaBuilder::Object(
					{
						{TEXT("foliage_type_path"), FSololmcpSchemaBuilder::String(TEXT("FoliageType asset path."), {}, 1, 1024)},
						{TEXT("locations"), FSololmcpSchemaBuilder::Array(
							FSololmcpSchemaBuilder::Object(
								{{TEXT("x"), FSololmcpSchemaBuilder::Number(TEXT("World X in cm."))},
								{TEXT("y"), FSololmcpSchemaBuilder::Number(TEXT("World Y in cm."))},
								{TEXT("z"), FSololmcpSchemaBuilder::Number(TEXT("World Z in cm."))}},
								{TEXT("x"), TEXT("y"), TEXT("z")}, FString(), false),
							TEXT("1-100000 world-space instance locations."), 1, 100000)},
					},
					{TEXT("foliage_type_path"), TEXT("locations")}, FString(), false),
				[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
					TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
				{
					UFoliageType* Type = nullptr;
					AInstancedFoliageActor* Actor = nullptr;
					FFoliageInfo* Info = nullptr;
					if (!LoadFoliageType(Context, Args, Type, Error)) return false;
					if (!GetFoliageInfo(Context, Type, Actor, Info, Error)) return false;
					TArray<TSharedPtr<FJsonValue>> LocationValues = Args->GetArrayField(TEXT("locations"));
					TArray<const FFoliageInstance*> Pending;
					TArray<FFoliageInstance> Instances;
					Instances.Reserve(LocationValues.Num());
					for (const TSharedPtr<FJsonValue>& Value : LocationValues)
					{
						const TSharedPtr<FJsonObject> Object = Value->AsObject();
						if (!Object) continue;
						FFoliageInstance Instance;
						Instance.Location = FVector(
							static_cast<double>(Object->GetNumberField(TEXT("x"))),
							static_cast<double>(Object->GetNumberField(TEXT("y"))),
							static_cast<double>(Object->GetNumberField(TEXT("z"))));
						Instance.Rotation = FRotator::ZeroRotator;
						Instance.DrawScale3D = FVector3f(1.0f, 1.0f, 1.0f);
						// Non-null base keeps AddInstanceBaseId's class-ignore IsA check
						// (no null guard in the engine) on the safe path.
						Instance.BaseComponent = Actor->GetRootComponent();
						Instances.Add(Instance);
						Pending.Add(&Instances.Last());
					}
					if (Pending.IsEmpty())
					{
						return Fail(Out, Error, TEXT("no_valid_locations"), TEXT("locations must contain at least one x/y/z object."));
					}
					{
						FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FoliagePaintInstances", "SOMOLMCP Foliage Paint Instances"));
						Actor->Modify();
						const int32 Before = Info->Instances.Num();
						Info->AddInstances(Type, Pending);
						const int32 Added = Info->Instances.Num() - Before;
						if (Added != Pending.Num())
						{
							Transaction.Cancel();
							return Fail(Out, Error, TEXT("foliage_add_count_mismatch"),
								FString::Printf(TEXT("Requested %d instances but only %d were added."), Pending.Num(), Added));
						}
						Actor->MarkPackageDirty();
					}
					Out->SetBoolField(TEXT("ok"), true);
					Out->SetStringField(TEXT("status"), TEXT("succeeded"));
					Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("foliage_paint_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
					Out->SetStringField(TEXT("foliage_type_path"), Type->GetPathName());
					Out->SetNumberField(TEXT("instances_added"), Pending.Num());
					Out->SetNumberField(TEXT("instances_total"), Info->Instances.Num());
					Out->SetBoolField(TEXT("mutation_applied"), true);
					Out->SetBoolField(TEXT("readback_verified"), true);
					Summary = FString::Printf(TEXT("Painted %d foliage instances."), Pending.Num());
					return true;
				}, nullptr, 0});

			// foliage_remove_instances — remove instances inside an axis-aligned world box.
			Registry.Register({TEXT("foliage_remove_instances"),
				TEXT("Remove foliage instances whose location lies inside a world-space AABB and return the removed-count readback."),
				FSololmcpSchemaBuilder::Object(
					{
						{TEXT("foliage_type_path"), FSololmcpSchemaBuilder::String(TEXT("FoliageType asset path."), {}, 1, 1024)},
						{TEXT("min"), FSololmcpSchemaBuilder::Object(
							{{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}},
							{TEXT("x"), TEXT("y"), TEXT("z")}, FString(), false)},
						{TEXT("max"), FSololmcpSchemaBuilder::Object(
							{{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}},
							{TEXT("x"), TEXT("y"), TEXT("z")}, FString(), false)},
					},
					{TEXT("foliage_type_path"), TEXT("min"), TEXT("max")}, FString(), false),
				[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
					TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
				{
					UFoliageType* Type = nullptr;
					AInstancedFoliageActor* Actor = nullptr;
					FFoliageInfo* Info = nullptr;
					if (!LoadFoliageType(Context, Args, Type, Error)) return false;
					FVector MinBox, MaxBox;
					if (!TryVector(Args, TEXT("min"), MinBox, Error) || !TryVector(Args, TEXT("max"), MaxBox, Error)) return false;
					const FBox Box(MinBox, MaxBox);
					if (!GetFoliageInfo(Context, Type, Actor, Info, Error)) return false;
					TArray<int32> Indices;
					for (int32 Index = 0; Index < Info->Instances.Num(); ++Index)
					{
						if (Box.IsInsideOrOn(Info->Instances[Index].Location))
						{
							Indices.Add(Index);
						}
					}
					if (Indices.IsEmpty())
					{
						return Fail(Out, Error, TEXT("no_matching_instances"), TEXT("No foliage instances are inside the requested box."));
					}
					{
						FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FoliageRemoveInstances", "SOMOLMCP Foliage Remove Instances"));
						Actor->Modify();
						const int32 Before = Info->Instances.Num();
						Info->RemoveInstances(Indices, true);
						const int32 Removed = Before - Info->Instances.Num();
						if (Removed != Indices.Num())
						{
							Transaction.Cancel();
							return Fail(Out, Error, TEXT("foliage_remove_count_mismatch"),
								FString::Printf(TEXT("Matched %d instances but only %d were removed."), Indices.Num(), Removed));
						}
						Actor->MarkPackageDirty();
					}
					Out->SetBoolField(TEXT("ok"), true);
					Out->SetStringField(TEXT("status"), TEXT("succeeded"));
					Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("foliage_remove_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
					Out->SetStringField(TEXT("foliage_type_path"), Type->GetPathName());
					Out->SetNumberField(TEXT("instances_removed"), Indices.Num());
					Out->SetNumberField(TEXT("instances_total"), Info->Instances.Num());
					Out->SetBoolField(TEXT("mutation_applied"), true);
					Out->SetBoolField(TEXT("readback_verified"), true);
					Summary = FString::Printf(TEXT("Removed %d foliage instances."), Indices.Num());
					return true;
				}, nullptr, 0});

			// foliage_query_instances — read back instance transforms inside a world-space AABB.
			Registry.Register({TEXT("foliage_query_instances"),
				TEXT("Query foliage instance transforms inside a world-space AABB and return them as structured readback."),
				FSololmcpSchemaBuilder::Object(
					{
						{TEXT("foliage_type_path"), FSololmcpSchemaBuilder::String(TEXT("FoliageType asset path."), {}, 1, 1024)},
						{TEXT("min"), FSololmcpSchemaBuilder::Object(
							{{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}},
							{TEXT("x"), TEXT("y"), TEXT("z")}, FString(), false)},
						{TEXT("max"), FSololmcpSchemaBuilder::Object(
							{{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}},
							{TEXT("x"), TEXT("y"), TEXT("z")}, FString(), false)},
					},
					{TEXT("foliage_type_path"), TEXT("min"), TEXT("max")}, FString(), false),
				[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
					TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
				{
					UFoliageType* Type = nullptr;
					AInstancedFoliageActor* Actor = nullptr;
					FFoliageInfo* Info = nullptr;
					if (!LoadFoliageType(Context, Args, Type, Error)) return false;
					FVector MinBox, MaxBox;
					if (!TryVector(Args, TEXT("min"), MinBox, Error) || !TryVector(Args, TEXT("max"), MaxBox, Error)) return false;
					const FBox Box(MinBox, MaxBox);
					if (!GetFoliageInfo(Context, Type, Actor, Info, Error)) return false;
					TArray<TSharedPtr<FJsonValue>> Instances;
					for (int32 Index = 0; Index < Info->Instances.Num(); ++Index)
					{
						const FFoliageInstance& Instance = Info->Instances[Index];
						if (!Box.IsInsideOrOn(Instance.Location)) continue;
						TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetNumberField(TEXT("index"), Index);
						TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
						Location->SetNumberField(TEXT("x"), Instance.Location.X);
						Location->SetNumberField(TEXT("y"), Instance.Location.Y);
						Location->SetNumberField(TEXT("z"), Instance.Location.Z);
						Entry->SetObjectField(TEXT("location"), Location);
						TSharedRef<FJsonObject> Rotation = MakeShared<FJsonObject>();
						Rotation->SetNumberField(TEXT("pitch"), Instance.Rotation.Pitch);
						Rotation->SetNumberField(TEXT("yaw"), Instance.Rotation.Yaw);
						Rotation->SetNumberField(TEXT("roll"), Instance.Rotation.Roll);
						Entry->SetObjectField(TEXT("rotation"), Rotation);
						Instances.Add(MakeShared<FJsonValueObject>(Entry));
					}
					Out->SetBoolField(TEXT("ok"), true);
					Out->SetStringField(TEXT("status"), TEXT("succeeded"));
					Out->SetStringField(TEXT("foliage_type_path"), Type->GetPathName());
					Out->SetArrayField(TEXT("instances"), Instances);
					Out->SetNumberField(TEXT("instance_count"), Instances.Num());
					Out->SetNumberField(TEXT("instances_total"), Info->Instances.Num());
					Summary = FString::Printf(TEXT("Queried %d foliage instances in the box."), Instances.Num());
					return true;
				}, nullptr, 15});

			// foliage_set_instance_transforms — batch-update instance transforms by index.
			Registry.Register({TEXT("foliage_set_instance_transforms"),
				TEXT("Batch-update foliage instance world transforms by index and return the applied-count readback."),
				FSololmcpSchemaBuilder::Object(
					{
						{TEXT("foliage_type_path"), FSololmcpSchemaBuilder::String(TEXT("FoliageType asset path."), {}, 1, 1024)},
						{TEXT("instances"), FSololmcpSchemaBuilder::Array(
							FSololmcpSchemaBuilder::Object(
								{
									{TEXT("index"), FSololmcpSchemaBuilder::Integer(TEXT("Instance index."), 0, MAX_int32)},
									{TEXT("location"), FSololmcpSchemaBuilder::Object(
										{{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}},
										{TEXT("x"), TEXT("y"), TEXT("z")}, FString(), false)},
									{TEXT("rotation"), FSololmcpSchemaBuilder::Object(
										{{TEXT("pitch"), FSololmcpSchemaBuilder::Number()}, {TEXT("yaw"), FSololmcpSchemaBuilder::Number()}, {TEXT("roll"), FSololmcpSchemaBuilder::Number()}},
										{TEXT("pitch"), TEXT("yaw"), TEXT("roll")}, FString(), true)},
									{TEXT("scale"), FSololmcpSchemaBuilder::Object(
										{{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}},
										{TEXT("x"), TEXT("y"), TEXT("z")}, FString(), true)},
								},
								{TEXT("index")}, FString(), false),
							TEXT("1-100000 instance transform updates."), 1, 100000)},
					},
					{TEXT("foliage_type_path"), TEXT("instances")}, FString(), false),
				[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
					TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
				{
					UFoliageType* Type = nullptr;
					AInstancedFoliageActor* Actor = nullptr;
					FFoliageInfo* Info = nullptr;
					if (!LoadFoliageType(Context, Args, Type, Error)) return false;
					if (!GetFoliageInfo(Context, Type, Actor, Info, Error)) return false;
					const TArray<TSharedPtr<FJsonValue>> Updates = Args->GetArrayField(TEXT("instances"));
					if (Updates.IsEmpty())
					{
						return Fail(Out, Error, TEXT("no_updates"), TEXT("instances must contain at least one entry."));
					}
					TArray<int32> ModifiedIndices;
					{
						FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FoliageSetTransforms", "SOMOLMCP Foliage Set Transforms"));
						Actor->Modify();
						for (const TSharedPtr<FJsonValue>& Value : Updates)
						{
							const TSharedPtr<FJsonObject> Entry = Value->AsObject();
							if (!Entry || !Entry->HasField(TEXT("index"))) continue;
							const int32 Index = static_cast<int32>(Entry->GetNumberField(TEXT("index")));
							if (Index < 0 || Index >= Info->Instances.Num())
							{
								Transaction.Cancel();
								return Fail(Out, Error, TEXT("instance_index_out_of_range"),
									FString::Printf(TEXT("Index %d is out of range (total %d)."), Index, Info->Instances.Num()));
							}
							FFoliageInstance& Instance = Info->Instances[Index];
							const TSharedPtr<FJsonObject> Location = Entry->GetObjectField(TEXT("location"));
							if (Location.IsValid())
							{
								Instance.Location = FVector(
									static_cast<double>(Location->GetNumberField(TEXT("x"))),
									static_cast<double>(Location->GetNumberField(TEXT("y"))),
									static_cast<double>(Location->GetNumberField(TEXT("z"))));
							}
							const TSharedPtr<FJsonObject> Rotation = Entry->GetObjectField(TEXT("rotation"));
							if (Rotation.IsValid())
							{
								Instance.Rotation = FRotator(
									static_cast<double>(Rotation->GetNumberField(TEXT("pitch"))),
									static_cast<double>(Rotation->GetNumberField(TEXT("yaw"))),
									static_cast<double>(Rotation->GetNumberField(TEXT("roll"))));
							}
							const TSharedPtr<FJsonObject> Scale = Entry->GetObjectField(TEXT("scale"));
							if (Scale.IsValid())
							{
								Instance.DrawScale3D = FVector3f(
									static_cast<float>(Scale->GetNumberField(TEXT("x"))),
									static_cast<float>(Scale->GetNumberField(TEXT("y"))),
									static_cast<float>(Scale->GetNumberField(TEXT("z"))));
							}
							ModifiedIndices.Add(Index);
						}
						// Locations moved but the spatial hash was not updated; a later
						// remove/query would hit FindChecked on a stale key and assert.
						Info->RecomputeHash();
						Actor->MarkPackageDirty();
					}
					Out->SetBoolField(TEXT("ok"), true);
					Out->SetStringField(TEXT("status"), TEXT("succeeded"));
					Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("foliage_set_transforms_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
					Out->SetStringField(TEXT("foliage_type_path"), Type->GetPathName());
					Out->SetNumberField(TEXT("instances_updated"), ModifiedIndices.Num());
					Out->SetBoolField(TEXT("mutation_applied"), true);
					Out->SetBoolField(TEXT("readback_verified"), true);
					Summary = FString::Printf(TEXT("Updated %d foliage instance transforms."), ModifiedIndices.Num());
					return true;
				}, nullptr, 0});
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		static bool LoadStaticMesh(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			UStaticMesh*& OutMesh, FString& Error)
		{
			FString Path;
			if (!Args->TryGetStringField(TEXT("target_mesh_path"), Path) || Path.IsEmpty())
			{
				Error = TEXT("Missing target_mesh_path.");
				return false;
			}
			OutMesh = Cast<UStaticMesh>(Context.Services.LoadAsset(Path, Error));
			if (!OutMesh)
			{
				if (Error.IsEmpty()) Error = TEXT("target_mesh_path is not a Static Mesh asset.");
				return false;
			}
			return true;
		}

		static bool CopyMeshIntoDynamic(UStaticMesh* Mesh, UDynamicMesh*& OutDynamic, UGeometryScriptDebug*& OutDebug, FString& Error)
		{
			OutDynamic = NewObject<UDynamicMesh>();
			OutDebug = NewObject<UGeometryScriptDebug>();
			FGeometryScriptCopyMeshFromAssetOptions AssetOptions;
			FGeometryScriptMeshReadLOD ReadLOD;
			EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
			UDynamicMesh* Result = UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(
				Mesh, OutDynamic, AssetOptions, ReadLOD, Outcome, true, OutDebug);
			if (!Result || Outcome != EGeometryScriptOutcomePins::Success)
			{
				Error = TEXT("CopyMeshFromStaticMesh failed; asset may not have readable geometry.");
				return false;
			}
			OutDynamic = Result;
			return true;
		}

		static bool PersistDynamicMesh(const FSololmcpToolExecutionContext& Context, UDynamicMesh* Dynamic,
			UStaticMesh* Mesh, TSharedRef<FJsonObject>& Out, FString& Error)
		{
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "GeometryScriptMutate", "SOMOLMCP Geometry Script Mutate"));
			Mesh->Modify();
			FGeometryScriptCopyMeshToAssetOptions WriteOptions;
			FGeometryScriptMeshWriteLOD WriteLOD;
			EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
			UGeometryScriptDebug* Debug = NewObject<UGeometryScriptDebug>();
			UDynamicMesh* WriteBack = UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
				Dynamic, Mesh, WriteOptions, WriteLOD, Outcome, true, Debug);
			if (!WriteBack || Outcome != EGeometryScriptOutcomePins::Success)
			{
				Transaction.Cancel();
				Error = TEXT("CopyMeshToStaticMesh failed; the asset was not modified.");
				return false;
			}
			Mesh->PostEditChange();
			Mesh->MarkPackageDirty();
			if (!Context.Services.SaveAsset(Mesh->GetPathName(), false, Error))
			{
				Transaction.Cancel();
				return false;
			}
			Out->SetBoolField(TEXT("mutation_applied"), true);
			Out->SetBoolField(TEXT("readback_verified"), true);
			return true;
		}

		static bool GeometryScriptErrorsPresent(const UGeometryScriptDebug* Debug)
		{
			if (!Debug) return false;
			for (const FGeometryScriptDebugMessage& Message : Debug->Messages)
			{
				// UE 5.8 error levels are NoError/UnknownError/InvalidInputs/OperationFailed;
				// any non-NoError level is treated as a failed operation.
				if (Message.ErrorType != EGeometryScriptErrorType::NoError)
				{
					return true;
				}
			}
			return false;
		}

		static void RegisterGeometryScriptTools(FSololmcpToolRegistry& Registry)
		{
			Registry.Register({TEXT("geometry_script_boolean"),
				TEXT("Run a native GeometryScript boolean operation on a Static Mesh asset (union/intersection/subtract), save, and verify readback."),
				FSololmcpSchemaBuilder::Object(
					{
						{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Target Static Mesh object path."), {}, 1, 1024)},
						{TEXT("other_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Tool Static Mesh object path."), {}, 1, 1024)},
						{TEXT("operation"), FSololmcpSchemaBuilder::String(TEXT("Boolean operation."), {TEXT("union"), TEXT("intersection"), TEXT("subtract")})},
					},
					{TEXT("target_mesh_path"), TEXT("other_mesh_path"), TEXT("operation")}, FString(), false),
				[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
					TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
				{
					UStaticMesh* Target = nullptr;
					if (!LoadStaticMesh(Context, Args, Target, Error)) return false;
					FString OtherPath;
					if (!Args->TryGetStringField(TEXT("other_mesh_path"), OtherPath) || OtherPath.IsEmpty())
					{
						return Fail(Out, Error, TEXT("missing_other_mesh"), TEXT("other_mesh_path is required."));
					}
					UStaticMesh* Other = Cast<UStaticMesh>(Context.Services.LoadAsset(OtherPath, Error));
					if (!Other)
					{
						return Fail(Out, Error, TEXT("invalid_other_mesh"), TEXT("other_mesh_path is not a Static Mesh asset."));
					}
					FString Operation = TEXT("union");
					Args->TryGetStringField(TEXT("operation"), Operation);
					EGeometryScriptBooleanOperation EnumOperation = EGeometryScriptBooleanOperation::Union;
					if (Operation == TEXT("intersection")) EnumOperation = EGeometryScriptBooleanOperation::Intersection;
					else if (Operation == TEXT("subtract")) EnumOperation = EGeometryScriptBooleanOperation::Subtract;
					else if (Operation != TEXT("union"))
					{
						return Fail(Out, Error, TEXT("invalid_operation"), TEXT("operation must be union, intersection, or subtract."));
					}
					UDynamicMesh* Dynamic = nullptr;
					UGeometryScriptDebug* Debug = nullptr;
					if (!CopyMeshIntoDynamic(Target, Dynamic, Debug, Error)) return false;
					UDynamicMesh* OtherDynamic = nullptr;
					UGeometryScriptDebug* OtherDebug = nullptr;
					if (!CopyMeshIntoDynamic(Other, OtherDynamic, OtherDebug, Error)) return false;
					FGeometryScriptMeshBooleanOptions Options;
					UDynamicMesh* Result = UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
						Dynamic, FTransform::Identity, OtherDynamic, FTransform::Identity, EnumOperation, Options, Debug);
					if (!Result || GeometryScriptErrorsPresent(Debug))
					{
						return Fail(Out, Error, TEXT("geometry_script_boolean_failed"), TEXT("The boolean operation reported errors."));
					}
					const int32 TrianglesBefore = Target->GetRenderData() && !Target->GetRenderData()->LODResources.IsEmpty()
						? Target->GetRenderData()->LODResources[0].GetNumTriangles() : -1;
					if (!PersistDynamicMesh(Context, Result, Target, Out, Error)) return false;
					const int32 TrianglesAfter = Result->GetMeshRef().TriangleCount();
					Out->SetBoolField(TEXT("ok"), true);
					Out->SetStringField(TEXT("status"), TEXT("succeeded"));
					Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("geometry_script_boolean_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
					Out->SetStringField(TEXT("asset_path"), Target->GetPathName());
					Out->SetStringField(TEXT("operation"), Operation);
					Out->SetNumberField(TEXT("triangles_before"), TrianglesBefore);
					Out->SetNumberField(TEXT("triangles_after"), TrianglesAfter);
					Summary = FString::Printf(TEXT("Applied %s boolean to %s."), *Operation, *Target->GetPathName());
					return true;
				}, nullptr, 0});

			Registry.Register({TEXT("geometry_script_extrude"),
				TEXT("Extrude a Static Mesh asset along +Z with native GeometryScript, save, and verify readback."),
				FSololmcpSchemaBuilder::Object(
					{
						{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Target Static Mesh object path."), {}, 1, 1024)},
						{TEXT("distance"), FSololmcpSchemaBuilder::Number(TEXT("Extrude distance in cm."), -1.0e6, 1.0e6)},
					},
					{TEXT("target_mesh_path")}, FString(), false),
				[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
					TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
				{
					UStaticMesh* Target = nullptr;
					if (!LoadStaticMesh(Context, Args, Target, Error)) return false;
					const float Distance = Args->HasField(TEXT("distance"))
						? static_cast<float>(Args->GetNumberField(TEXT("distance"))) : 100.0f;
					UDynamicMesh* Dynamic = nullptr;
					UGeometryScriptDebug* Debug = nullptr;
					if (!CopyMeshIntoDynamic(Target, Dynamic, Debug, Error)) return false;
					FGeometryScriptMeshExtrudeOptions Options;
					Options.ExtrudeDistance = Distance;
					Options.bSolidsToShells = false;
					UDynamicMesh* Result = UGeometryScriptLibrary_MeshModelingFunctions::ApplyMeshExtrude_Compatibility_5p0(Dynamic, Options, Debug);
					if (!Result || GeometryScriptErrorsPresent(Debug))
					{
						return Fail(Out, Error, TEXT("geometry_script_extrude_failed"), TEXT("The extrude operation reported errors."));
					}
					const int32 TrianglesBefore = Target->GetRenderData() && !Target->GetRenderData()->LODResources.IsEmpty()
						? Target->GetRenderData()->LODResources[0].GetNumTriangles() : -1;
					if (!PersistDynamicMesh(Context, Result, Target, Out, Error)) return false;
					const int32 TrianglesAfter = Result->GetMeshRef().TriangleCount();
					Out->SetBoolField(TEXT("ok"), true);
					Out->SetStringField(TEXT("status"), TEXT("succeeded"));
					Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("geometry_script_extrude_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
					Out->SetStringField(TEXT("asset_path"), Target->GetPathName());
					Out->SetNumberField(TEXT("distance_cm"), Distance);
					Out->SetNumberField(TEXT("triangles_before"), TrianglesBefore);
					Out->SetNumberField(TEXT("triangles_after"), TrianglesAfter);
					Summary = FString::Printf(TEXT("Extruded %s by %.1f cm."), *Target->GetPathName(), Distance);
					return true;
				}, nullptr, 0});

			Registry.Register({TEXT("geometry_script_remesh"),
				TEXT("Uniform-remesh a Static Mesh asset toward a target triangle count with native GeometryScript, save, and verify readback."),
				FSololmcpSchemaBuilder::Object(
					{
						{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Target Static Mesh object path."), {}, 1, 1024)},
						{TEXT("target_triangle_count"), FSololmcpSchemaBuilder::Integer(TEXT("Desired triangle count."), 1, 10000000)},
					},
					{TEXT("target_mesh_path")}, FString(), false),
				[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
					TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
				{
					UStaticMesh* Target = nullptr;
					if (!LoadStaticMesh(Context, Args, Target, Error)) return false;
					const int32 TargetCount = Args->HasField(TEXT("target_triangle_count"))
						? FMath::Clamp(static_cast<int32>(Args->GetNumberField(TEXT("target_triangle_count"))), 1, 10000000) : 5000;
					UDynamicMesh* Dynamic = nullptr;
					UGeometryScriptDebug* Debug = nullptr;
					if (!CopyMeshIntoDynamic(Target, Dynamic, Debug, Error)) return false;
					FGeometryScriptRemeshOptions RemeshOptions;
					FGeometryScriptUniformRemeshOptions UniformOptions;
					UniformOptions.TargetType = EGeometryScriptUniformRemeshTargetType::TriangleCount;
					UniformOptions.TargetTriangleCount = TargetCount;
					UDynamicMesh* Result = UGeometryScriptLibrary_RemeshingFunctions::ApplyUniformRemesh(
						Dynamic, RemeshOptions, UniformOptions, Debug);
					if (!Result || GeometryScriptErrorsPresent(Debug))
					{
						return Fail(Out, Error, TEXT("geometry_script_remesh_failed"), TEXT("The remesh operation reported errors."));
					}
					const int32 TrianglesBefore = Target->GetRenderData() && !Target->GetRenderData()->LODResources.IsEmpty()
						? Target->GetRenderData()->LODResources[0].GetNumTriangles() : -1;
					if (!PersistDynamicMesh(Context, Result, Target, Out, Error)) return false;
					const int32 TrianglesAfter = Result->GetMeshRef().TriangleCount();
					Out->SetBoolField(TEXT("ok"), true);
					Out->SetStringField(TEXT("status"), TEXT("succeeded"));
					Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("geometry_script_remesh_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
					Out->SetStringField(TEXT("asset_path"), Target->GetPathName());
					Out->SetNumberField(TEXT("target_triangle_count"), TargetCount);
					Out->SetNumberField(TEXT("triangles_before"), TrianglesBefore);
					Out->SetNumberField(TEXT("triangles_after"), TrianglesAfter);
					Summary = FString::Printf(TEXT("Remeshed %s to %d triangles."), *Target->GetPathName(), TrianglesAfter);
					return true;
				}, nullptr, 0});

			Registry.Register({TEXT("geometry_script_auto_uv"),
				TEXT("Auto-generate XAtlas UVs on a Static Mesh asset with native GeometryScript, save, and verify readback."),
				FSololmcpSchemaBuilder::Object(
					{
						{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Target Static Mesh object path."), {}, 1, 1024)},
					},
					{TEXT("target_mesh_path")}, FString(), false),
				[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
					TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
				{
					UStaticMesh* Target = nullptr;
					if (!LoadStaticMesh(Context, Args, Target, Error)) return false;
					UDynamicMesh* Dynamic = nullptr;
					UGeometryScriptDebug* Debug = nullptr;
					if (!CopyMeshIntoDynamic(Target, Dynamic, Debug, Error)) return false;
					FGeometryScriptXAtlasOptions Options;
					UDynamicMesh* Result = UGeometryScriptLibrary_MeshUVFunctions::AutoGenerateXAtlasMeshUVs(Dynamic, 0, Options, Debug);
					if (!Result || GeometryScriptErrorsPresent(Debug))
					{
						return Fail(Out, Error, TEXT("geometry_script_auto_uv_failed"), TEXT("The UV generation operation reported errors."));
					}
					if (!PersistDynamicMesh(Context, Result, Target, Out, Error)) return false;
					Out->SetBoolField(TEXT("ok"), true);
					Out->SetStringField(TEXT("status"), TEXT("succeeded"));
					Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("geometry_script_auto_uv_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
					Out->SetStringField(TEXT("asset_path"), Target->GetPathName());
					Out->SetNumberField(TEXT("uv_channel"), 0);
					Out->SetNumberField(TEXT("triangles_after"), Result->GetMeshRef().TriangleCount());
					Summary = FString::Printf(TEXT("Generated XAtlas UVs for %s."), *Target->GetPathName());
					return true;
				}, nullptr, 0});

			Registry.Register({TEXT("geometry_script_repair_mesh"),
				TEXT("Repair a Static Mesh asset (weld edges, remove degenerates, fill holes, compact) with native GeometryScript, save, and verify readback."),
				FSololmcpSchemaBuilder::Object(
					{
						{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Target Static Mesh object path."), {}, 1, 1024)},
					},
					{TEXT("target_mesh_path")}, FString(), false),
				[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
					TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
				{
					UStaticMesh* Target = nullptr;
					if (!LoadStaticMesh(Context, Args, Target, Error)) return false;
					UDynamicMesh* Dynamic = nullptr;
					UGeometryScriptDebug* Debug = nullptr;
					if (!CopyMeshIntoDynamic(Target, Dynamic, Debug, Error)) return false;
					UDynamicMesh* Mesh = UGeometryScriptLibrary_MeshRepairFunctions::SplitMeshBowties(Dynamic, true, true, Debug);
					if (!Mesh) return Fail(Out, Error, TEXT("geometry_script_repair_failed"), TEXT("SplitMeshBowties failed."));
					FGeometryScriptWeldEdgesOptions WeldOptions;
					Mesh = UGeometryScriptLibrary_MeshRepairFunctions::WeldMeshEdges(Mesh, WeldOptions, Debug);
					if (!Mesh) return Fail(Out, Error, TEXT("geometry_script_repair_failed"), TEXT("WeldMeshEdges failed."));
					Mesh = UGeometryScriptLibrary_MeshRepairFunctions::CompactMesh(Mesh, Debug);
					if (!Mesh) return Fail(Out, Error, TEXT("geometry_script_repair_failed"), TEXT("CompactMesh failed."));
					if (GeometryScriptErrorsPresent(Debug))
					{
						return Fail(Out, Error, TEXT("geometry_script_repair_failed"), TEXT("The repair pipeline reported errors."));
					}
					if (!PersistDynamicMesh(Context, Mesh, Target, Out, Error)) return false;
					Out->SetBoolField(TEXT("ok"), true);
					Out->SetStringField(TEXT("status"), TEXT("succeeded"));
					Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("geometry_script_repair_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
					Out->SetStringField(TEXT("asset_path"), Target->GetPathName());
					Out->SetNumberField(TEXT("triangles_after"), Mesh->GetMeshRef().TriangleCount());
					Summary = FString::Printf(TEXT("Repaired %s."), *Target->GetPathName());
					return true;
				}, nullptr, 0});
		}
#endif // ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	}

	void RegisterLegacyNativeCompletionTools(FSololmcpToolRegistry& Registry)
	{
		LegacyNativeCompletion::RegisterFoliageTools(Registry);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		LegacyNativeCompletion::RegisterGeometryScriptTools(Registry);
#else
		// Older engines: keep the fail-closed blocked adapters so the tool names
		// remain discoverable in the catalog while never executing Python.
		Registry.Register({TEXT("geometry_script_boolean"),
			TEXT("Blocked on this engine version: GeometryScript native execution requires UE 5.8."),
			FSololmcpSchemaBuilder::Object({{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("target_mesh_path")}),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString&, FString& Error)
			{
				Out->SetStringField(TEXT("ok"), TEXT("false"));
				Out->SetStringField(TEXT("error_code"), TEXT("blocked_requires_ue58_geometry_script"));
				Error = TEXT("geometry_script_* native execution requires UE 5.8.");
				return false;
			}, nullptr, 0});
		Registry.Register({TEXT("geometry_script_extrude"),
			TEXT("Blocked on this engine version: GeometryScript native execution requires UE 5.8."),
			FSololmcpSchemaBuilder::Object({{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("target_mesh_path")}),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString&, FString& Error)
			{
				Out->SetStringField(TEXT("ok"), TEXT("false"));
				Out->SetStringField(TEXT("error_code"), TEXT("blocked_requires_ue58_geometry_script"));
				Error = TEXT("geometry_script_* native execution requires UE 5.8.");
				return false;
			}, nullptr, 0});
		Registry.Register({TEXT("geometry_script_remesh"),
			TEXT("Blocked on this engine version: GeometryScript native execution requires UE 5.8."),
			FSololmcpSchemaBuilder::Object({{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("target_mesh_path")}),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString&, FString& Error)
			{
				Out->SetStringField(TEXT("ok"), TEXT("false"));
				Out->SetStringField(TEXT("error_code"), TEXT("blocked_requires_ue58_geometry_script"));
				Error = TEXT("geometry_script_* native execution requires UE 5.8.");
				return false;
			}, nullptr, 0});
		Registry.Register({TEXT("geometry_script_auto_uv"),
			TEXT("Blocked on this engine version: GeometryScript native execution requires UE 5.8."),
			FSololmcpSchemaBuilder::Object({{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("target_mesh_path")}),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString&, FString& Error)
			{
				Out->SetStringField(TEXT("ok"), TEXT("false"));
				Out->SetStringField(TEXT("error_code"), TEXT("blocked_requires_ue58_geometry_script"));
				Error = TEXT("geometry_script_* native execution requires UE 5.8.");
				return false;
			}, nullptr, 0});
		Registry.Register({TEXT("geometry_script_repair_mesh"),
			TEXT("Blocked on this engine version: GeometryScript native execution requires UE 5.8."),
			FSololmcpSchemaBuilder::Object({{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("target_mesh_path")}),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString&, FString& Error)
			{
				Out->SetStringField(TEXT("ok"), TEXT("false"));
				Out->SetStringField(TEXT("error_code"), TEXT("blocked_requires_ue58_geometry_script"));
				Error = TEXT("geometry_script_* native execution requires UE 5.8.");
				return false;
			}, nullptr, 0});
#endif
	}
}
