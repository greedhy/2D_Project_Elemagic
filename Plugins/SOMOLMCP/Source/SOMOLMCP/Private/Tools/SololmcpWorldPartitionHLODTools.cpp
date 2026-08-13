// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpWorldPartitionHLODTools.cpp
// P3-1 -World Partition HLOD Advanced Tools (5 tools)
//
// Tool prefix: worldpartition_hlod_*
//   1. worldpartition_hlod_layer_create     -Create UHLODLayer asset
//   2. worldpartition_hlod_layer_set        -Update layer settings
//   3. worldpartition_hlod_layer_inspect    -Read layer settings
//   4. worldpartition_hlod_actor_set_layer  -Assign HLODLayer to an actor
//   5. worldpartition_hlod_force_rebuild    -Trigger HLOD rebuild through the editor console command path
//
// Editor module deps (already in SOMOLMCP.Build.cs):
//   - WorldPartitionEditor       (UWorldPartition + editor builders)
//   - Engine                      (UHLODLayer is in Runtime/Engine/Public/WorldPartition/HLOD)
//   - AssetRegistry               (FAssetRegistryModule::AssetCreated)
//   - UnrealEd                    (FScopedTransaction)
//
// TODO(P3-1) audit:
//   * UHLODLayer setters (CellSize / LoadingRange / RuntimeGrid / HLODBuilderClass) are
//     mostly UPROPERTY edit-anywhere fields. We assign via property reflection rather
//     than direct typed setters because exact field names vary across UE 5.x revisions.
//     The reflection path keeps this compileable even if a specific field renames.
//   * worldpartition_hlod_force_rebuild: programmatic builder invocation
//     (FWorldPartitionHLODsBuilder / IWorldPartitionEditorModule::RunBuilder)
//     requires WorldPartitionHLODUtilities + commandlet plumbing that is too
//     internal for runtime use. The production path therefore dispatches the
//     verified editor console command and returns an async receipt; filtered
//     cell rebuilds remain fail-closed because the console path cannot prove
//     that the filter was honored.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/HLOD/HLODLayer.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include <type_traits>

#define LOCTEXT_NAMESPACE "SOMOLMCP_WorldPartitionHLOD"

namespace UE::SOMOLMCP
{
	namespace WPHLOD
	{
		using SB = FSololmcpSchemaBuilder;

		// Resolve world by path with editor-world fallback (mirrors WP basic tools).
		static UWorld* ResolveWorld(const FSololmcpEditorServices& Services,
			const FString& WorldPath, FString& OutErr)
		{
			UWorld* Editor = Services.GetEditorWorld(OutErr);
			if (WorldPath.IsEmpty()) { return Editor; }

			if (Editor)
			{
				const FString EditorPath = Editor->GetPathName();
				if (EditorPath == WorldPath || Editor->GetName() == WorldPath
				    || EditorPath.StartsWith(WorldPath))
				{
					return Editor;
				}
			}
			UWorld* Loaded = LoadObject<UWorld>(nullptr, *WorldPath);
			if (!Loaded)
			{
				OutErr = FString::Printf(TEXT("World '%s' not found / not loaded."), *WorldPath);
				return Editor;
			}
			return Loaded;
		}

		static AActor* ResolveActor(UWorld* World, const FString& Id)
		{
			if (!World) return nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if ((*It)->GetPathName() == Id) return *It;
			}
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if ((*It)->GetName() == Id) return *It;
			}
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if ((*It)->GetActorLabel() == Id) return *It;
			}
			return nullptr;
		}

		static bool ConsoleTextLooksLikeError(const FString& Text)
		{
			return Text.Contains(TEXT("error"), ESearchCase::IgnoreCase)
				|| Text.Contains(TEXT("failed"), ESearchCase::IgnoreCase)
				|| Text.Contains(TEXT("unknown command"), ESearchCase::IgnoreCase);
		}

		// Best-effort property setter via reflection. Returns true if a property
		// with `PropertyName` was found on `Obj`'s class and was assigned.
		// Supported value types: float, double, int32, uint32, bool, FName, FString.
		template <typename TValue>
		static bool SetReflectedProperty(UObject* Obj, FName PropertyName, const TValue& Value)
		{
			if (!Obj) return false;
			FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropertyName);
			if (!Prop) return false;

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);

			// Numeric properties.
			if (FNumericProperty* Num = CastField<FNumericProperty>(Prop))
			{
				if constexpr (std::is_floating_point_v<TValue>)
				{
					Num->SetFloatingPointPropertyValue(ValuePtr, static_cast<double>(Value));
					return true;
				}
				else if constexpr (std::is_integral_v<TValue>)
				{
					Num->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value));
					return true;
				}
			}
			// Bool.
			if (FBoolProperty* B = CastField<FBoolProperty>(Prop))
			{
				if constexpr (std::is_integral_v<TValue> || std::is_same_v<TValue, bool>)
				{
					B->SetPropertyValue(ValuePtr, static_cast<bool>(Value));
					return true;
				}
			}
			// FName.
			if (FNameProperty* N = CastField<FNameProperty>(Prop))
			{
				if constexpr (std::is_same_v<TValue, FName>)
				{
					N->SetPropertyValue(ValuePtr, Value);
					return true;
				}
				else if constexpr (std::is_same_v<TValue, FString>)
				{
					N->SetPropertyValue(ValuePtr, FName(*Value));
					return true;
				}
			}
			// FString.
			if (FStrProperty* S = CastField<FStrProperty>(Prop))
			{
				if constexpr (std::is_same_v<TValue, FString>)
				{
					S->SetPropertyValue(ValuePtr, Value);
					return true;
				}
				else if constexpr (std::is_same_v<TValue, FName>)
				{
					S->SetPropertyValue(ValuePtr, Value.ToString());
					return true;
				}
			}
			return false;
		}

		// Best-effort property getters.
		static bool GetReflectedFloat(UObject* Obj, FName PropertyName, double& Out)
		{
			if (!Obj) return false;
			FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropertyName);
			if (!Prop) return false;
			if (FNumericProperty* Num = CastField<FNumericProperty>(Prop))
			{
				Out = Num->GetFloatingPointPropertyValue(Prop->ContainerPtrToValuePtr<void>(Obj));
				return true;
			}
			return false;
		}

		static bool GetReflectedInt(UObject* Obj, FName PropertyName, int64& Out)
		{
			if (!Obj) return false;
			FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropertyName);
			if (!Prop) return false;
			if (FNumericProperty* Num = CastField<FNumericProperty>(Prop))
			{
				Out = Num->GetSignedIntPropertyValue(Prop->ContainerPtrToValuePtr<void>(Obj));
				return true;
			}
			return false;
		}

		static bool GetReflectedName(UObject* Obj, FName PropertyName, FName& Out)
		{
			if (!Obj) return false;
			FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropertyName);
			if (!Prop) return false;
			if (FNameProperty* N = CastField<FNameProperty>(Prop))
			{
				Out = N->GetPropertyValue(Prop->ContainerPtrToValuePtr<void>(Obj));
				return true;
			}
			if (FStrProperty* S = CastField<FStrProperty>(Prop))
			{
				Out = FName(*S->GetPropertyValue(Prop->ContainerPtrToValuePtr<void>(Obj)));
				return true;
			}
			return false;
		}

		// Map "merged_mesh"/"instanced_mesh"/"approximate" toan enum-like int value
		// the user can read back.
		static int32 BuildMethodToInt(const FString& Method)
		{
			if (Method.Equals(TEXT("instanced_mesh"), ESearchCase::IgnoreCase)) return 1;
			if (Method.Equals(TEXT("approximate"),    ESearchCase::IgnoreCase)) return 2;
			return 0; // merged_mesh (default)
		}

		static FString IntToBuildMethod(int32 V)
		{
			switch (V)
			{
				case 1: return TEXT("instanced_mesh");
				case 2: return TEXT("approximate");
				default: return TEXT("merged_mesh");
			}
		}

	} // namespace WPHLOD

	// ════════════════════════════════════════════════════════════════════
	void RegisterWorldPartitionHLODTools(FSololmcpToolRegistry& R)
	{
		using namespace WPHLOD;

		// ── 1. worldpartition_hlod_layer_create ───────────────────────
		R.Register({
			TEXT("worldpartition_hlod_layer_create"),
			TEXT("Create a new UHLODLayer asset under the world's HLOD directory. "
			     "Configures basic settings (screen_size, build_method)."),
			SB::Object({
				{TEXT("world_path"),    SB::String(TEXT("Level asset path. Empty = current editor world."))},
				{TEXT("layer_name"),    SB::String(TEXT("Asset name (without path), e.g. 'HLOD_Far'."))},
				{TEXT("screen_size"),   SB::Number(TEXT("Screen size threshold (default 0.5)."))},
				{TEXT("build_method"),  SB::String(TEXT("'merged_mesh'|'instanced_mesh'|'approximate' (default 'merged_mesh')."),
								{TEXT("merged_mesh"), TEXT("instanced_mesh"), TEXT("approximate")})}
			}, {TEXT("layer_name")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString WorldPath; Args->TryGetStringField(TEXT("world_path"), WorldPath);
				FString LayerName;
				if (!Args->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
				{
					SololmcpError::MissingParam(Out, TEXT("layer_name"));
					Error = TEXT("Missing layer_name."); return false;
				}
				const double ScreenSize = Args->HasTypedField<EJson::Number>(TEXT("screen_size"))
					? Args->GetNumberField(TEXT("screen_size")) : 0.5;
				FString BuildMethod = TEXT("merged_mesh");
				Args->TryGetStringField(TEXT("build_method"), BuildMethod);

				UWorld* World = ResolveWorld(Ctx.Services, WorldPath, Error);
				if (!World) { SololmcpError::NotFound(Out, WorldPath); return false; }

				// Derive an asset path under the world's HLOD directory.
				// e.g. /Game/Maps/MyLevel  to /Game/Maps/MyLevel_HLOD/HLOD_Far
				const FString WorldPkg = World->GetOutermost()->GetName();
				const FString PackageDir = FString::Printf(TEXT("%s_HLOD"), *WorldPkg);
				const FString AssetPath  = FString::Printf(TEXT("%s/%s"), *PackageDir, *LayerName);

				UPackage* Pkg = CreatePackage(*AssetPath);
				if (!Pkg)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						FString::Printf(TEXT("CreatePackage('%s') returned null."), *AssetPath));
					Error = TEXT("CreatePackage failed."); return false;
				}

				// Class collision pre-check.
				if (UObject* Existing = StaticFindObject(nullptr, Pkg, *LayerName))
				{
					if (!Existing->IsA<UHLODLayer>())
					{
						Error = FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing to replace with UHLODLayer."),
							*AssetPath, *Existing->GetClass()->GetName());
						SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""), Error);
						return false;
					}
				}

				const FScopedTransaction Tx(LOCTEXT("HLODLayerCreate", "SOMOLMCP HLOD Layer Create"));

				UHLODLayer* Layer = NewObject<UHLODLayer>(Pkg, *LayerName, RF_Public | RF_Standalone);
				if (!Layer)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("NewObject<UHLODLayer> returned null."));
					Error = TEXT("HLODLayer create failed."); return false;
				}
				Layer->Modify();

				// TODO(P3-1): exact UPROPERTY field name varies by UE 5.x. Try common
				// aliases via reflection so this compiles regardless.
				bool bSetScreenSize = false;
				bSetScreenSize |= SetReflectedProperty<double>(Layer, TEXT("CellOverlap"),    ScreenSize);
				bSetScreenSize |= SetReflectedProperty<double>(Layer, TEXT("ScreenSize"),     ScreenSize);
				bSetScreenSize |= SetReflectedProperty<double>(Layer, TEXT("MinVisibleScreenSize"), ScreenSize);

				const int32 BuildInt = BuildMethodToInt(BuildMethod);
				bool bSetBuildMethod = false;
				bSetBuildMethod |= SetReflectedProperty<int32>(Layer, TEXT("LayerType"), BuildInt);

				FAssetRegistryModule::AssetCreated(Layer);
				SololmcpWriteFlush::EnsureFlushed(Layer);

				Out->SetStringField(TEXT("layer_path"), Layer->GetPathName());
				Out->SetStringField(TEXT("layer_name"), LayerName);
				Out->SetNumberField(TEXT("screen_size"), ScreenSize);
				Out->SetStringField(TEXT("build_method"), BuildMethod);
				Out->SetBoolField(TEXT("screen_size_applied"),  bSetScreenSize);
				Out->SetBoolField(TEXT("build_method_applied"), bSetBuildMethod);

				Summary = FString::Printf(TEXT("HLODLayer '%s' created at %s (screen_size=%.3f, build=%s)."),
					*LayerName, *Layer->GetPathName(), ScreenSize, *BuildMethod);
				return true;
			},
			nullptr, 0
		});

		// ── 2. worldpartition_hlod_layer_set ──────────────────────────
		R.Register({
			TEXT("worldpartition_hlod_layer_set"),
			TEXT("Update an existing UHLODLayer's settings (screen_size, cell_size, runtime_grid). "
			     "Returns the list of fields that were actually applied."),
			SB::Object({
				{TEXT("layer_path"),   SB::String(TEXT("UHLODLayer asset path."))},
				{TEXT("screen_size"),  SB::Number(TEXT("Optional screen size threshold."))},
				{TEXT("cell_size"),    SB::Integer(TEXT("Optional cell size in cm (uint)."))},
				{TEXT("runtime_grid"), SB::String(TEXT("Optional runtime grid name."))}
			}, {TEXT("layer_path")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString LayerPath;
				if (!Args->TryGetStringField(TEXT("layer_path"), LayerPath) || LayerPath.IsEmpty())
				{
					SololmcpError::MissingParam(Out, TEXT("layer_path"));
					Error = TEXT("Missing layer_path."); return false;
				}

				UHLODLayer* Layer = LoadObject<UHLODLayer>(nullptr, *LayerPath);
				if (!Layer)
				{
					SololmcpError::InvalidPath(Out, LayerPath);
					Error = FString::Printf(TEXT("HLODLayer '%s' not found."), *LayerPath);
					return false;
				}

				const FScopedTransaction Tx(LOCTEXT("HLODLayerSet", "SOMOLMCP HLOD Layer Set"));
				Layer->Modify();

				TArray<TSharedPtr<FJsonValue>> Updated;

				// screen_size
				if (Args->HasTypedField<EJson::Number>(TEXT("screen_size")))
				{
					const double V = Args->GetNumberField(TEXT("screen_size"));
					bool bAny = false;
					bAny |= SetReflectedProperty<double>(Layer, TEXT("CellOverlap"),           V);
					bAny |= SetReflectedProperty<double>(Layer, TEXT("ScreenSize"),            V);
					bAny |= SetReflectedProperty<double>(Layer, TEXT("MinVisibleScreenSize"),  V);
					if (bAny) { Updated.Add(MakeShared<FJsonValueString>(TEXT("screen_size"))); }
				}

				// cell_size
				if (Args->HasTypedField<EJson::Number>(TEXT("cell_size")))
				{
					const int64 V = Args->GetIntegerField(TEXT("cell_size"));
					bool bAny = false;
					bAny |= SetReflectedProperty<int32>(Layer, TEXT("CellSize"),     static_cast<int32>(V));
					bAny |= SetReflectedProperty<int32>(Layer, TEXT("LoadingRange"), static_cast<int32>(V));
					if (bAny) { Updated.Add(MakeShared<FJsonValueString>(TEXT("cell_size"))); }
				}

				// runtime_grid
				if (Args->HasField(TEXT("runtime_grid")))
				{
					FString V; Args->TryGetStringField(TEXT("runtime_grid"), V);
					bool bAny = false;
					bAny |= SetReflectedProperty<FName>(Layer, TEXT("RuntimeGrid"), FName(*V));
					if (!bAny)
					{
						bAny |= SetReflectedProperty<FString>(Layer, TEXT("RuntimeGrid"), V);
					}
					if (bAny) { Updated.Add(MakeShared<FJsonValueString>(TEXT("runtime_grid"))); }
				}

				SololmcpWriteFlush::EnsureFlushed(Layer);

				Out->SetStringField(TEXT("layer_path"), Layer->GetPathName());
				Out->SetArrayField(TEXT("updated_fields"), Updated);

				Summary = FString::Printf(TEXT("HLODLayer '%s' updated (%d fields)."),
					*Layer->GetName(), Updated.Num());
				return true;
			},
			nullptr, 0
		});

		// ── 3. worldpartition_hlod_layer_inspect ──────────────────────
		R.Register({
			TEXT("worldpartition_hlod_layer_inspect"),
			TEXT("Inspect a UHLODLayer's current settings."),
			SB::Object({
				{TEXT("layer_path"), SB::String(TEXT("UHLODLayer asset path."))}
			}, {TEXT("layer_path")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString LayerPath;
				if (!Args->TryGetStringField(TEXT("layer_path"), LayerPath) || LayerPath.IsEmpty())
				{
					SololmcpError::MissingParam(Out, TEXT("layer_path"));
					Error = TEXT("Missing layer_path."); return false;
				}
				UHLODLayer* Layer = LoadObject<UHLODLayer>(nullptr, *LayerPath);
				if (!Layer)
				{
					SololmcpError::InvalidPath(Out, LayerPath);
					return false;
				}

				double ScreenSize = 0.0;
				if (!GetReflectedFloat(Layer, TEXT("ScreenSize"), ScreenSize))
				{
					if (!GetReflectedFloat(Layer, TEXT("MinVisibleScreenSize"), ScreenSize))
					{
						GetReflectedFloat(Layer, TEXT("CellOverlap"), ScreenSize);
					}
				}
				int64 CellSize = 0;
				if (!GetReflectedInt(Layer, TEXT("CellSize"), CellSize))
				{
					GetReflectedInt(Layer, TEXT("LoadingRange"), CellSize);
				}
				FName RuntimeGrid = NAME_None;
				GetReflectedName(Layer, TEXT("RuntimeGrid"), RuntimeGrid);

				int64 LayerTypeInt = -1;
				GetReflectedInt(Layer, TEXT("LayerType"), LayerTypeInt);
				const FString BuildMethod = (LayerTypeInt >= 0)
					? IntToBuildMethod(static_cast<int32>(LayerTypeInt))
					: TEXT("unknown");

				int32 MaterialsCount = 0;
				if (FArrayProperty* Arr = CastField<FArrayProperty>(
					Layer->GetClass()->FindPropertyByName(TEXT("Materials"))))
				{
					FScriptArrayHelper Helper(Arr, Arr->ContainerPtrToValuePtr<void>(Layer));
					MaterialsCount = Helper.Num();
				}

				Out->SetStringField(TEXT("layer_name"),     Layer->GetName());
				Out->SetStringField(TEXT("layer_path"),     Layer->GetPathName());
				Out->SetNumberField(TEXT("screen_size"),    ScreenSize);
				Out->SetNumberField(TEXT("cell_size"),      static_cast<double>(CellSize));
				Out->SetStringField(TEXT("build_method"),   BuildMethod);
				Out->SetStringField(TEXT("runtime_grid"),   RuntimeGrid.ToString());
				Out->SetNumberField(TEXT("materials_count"), MaterialsCount);

				Summary = FString::Printf(TEXT("HLODLayer '%s': screen=%.3f, cell=%lld, build=%s, grid=%s"),
					*Layer->GetName(), ScreenSize, CellSize, *BuildMethod, *RuntimeGrid.ToString());
				return true;
			},
			nullptr, 5
		});

		// ── 4. worldpartition_hlod_actor_set_layer ────────────────────
		R.Register({
			TEXT("worldpartition_hlod_actor_set_layer"),
			TEXT("Assign an HLODLayer to an actor (or clear it by passing an empty hlod_layer_path). "
			     "Uses property reflection to set the actor's HLODLayer pointer."),
			SB::Object({
				{TEXT("actor_name"),       SB::String(TEXT("Actor path/name/label."))},
				{TEXT("hlod_layer_path"),  SB::String(TEXT("UHLODLayer asset path; empty string clears assignment."))}
			}, {TEXT("actor_name")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString ActorId;
				if (!Args->TryGetStringField(TEXT("actor_name"), ActorId) || ActorId.IsEmpty())
				{
					SololmcpError::MissingParam(Out, TEXT("actor_name"));
					Error = TEXT("Missing actor_name."); return false;
				}
				FString LayerPath; Args->TryGetStringField(TEXT("hlod_layer_path"), LayerPath);

				UWorld* World = Ctx.Services.GetEditorWorld(Error);
				if (!World) return false;

				AActor* Actor = ResolveActor(World, ActorId);
				if (!Actor) { SololmcpError::NotFound(Out, ActorId); return false; }

				UHLODLayer* Layer = nullptr;
				if (!LayerPath.IsEmpty())
				{
					Layer = LoadObject<UHLODLayer>(nullptr, *LayerPath);
					if (!Layer)
					{
						SololmcpError::InvalidPath(Out, LayerPath);
						return false;
					}
				}

				const FScopedTransaction Tx(LOCTEXT("HLODActorSetLayer", "SOMOLMCP HLOD Actor Set Layer"));
				Actor->Modify();

				// AActor exposes an HLODLayer UPROPERTY in UE 5.x. Use reflection so we
				// don't need to know the exact accessor name (some classes shadow it).
				bool bApplied = false;
				UObject* Readback = nullptr;
				FObjectProperty* ObjProp = CastField<FObjectProperty>(
					Actor->GetClass()->FindPropertyByName(TEXT("HLODLayer")));
				if (ObjProp)
				{
					ObjProp->SetObjectPropertyValue_InContainer(Actor, Layer);
					Readback = ObjProp->GetObjectPropertyValue_InContainer(Actor);
					bApplied = (Readback == Layer);
				}

				SololmcpWriteFlush::EnsureFlushed(Actor);

				Out->SetStringField(TEXT("actor"), Actor->GetName());
				Out->SetStringField(TEXT("layer"), Layer ? Layer->GetPathName() : TEXT(""));
				Out->SetStringField(TEXT("readback_layer"), Readback ? Readback->GetPathName() : TEXT(""));
				Out->SetBoolField(TEXT("applied"), bApplied);
				if (!bApplied)
				{
					Out->SetStringField(TEXT("warning"),
						ObjProp
							? TEXT("Actor HLODLayer readback did not match requested layer.")
							: TEXT("Actor class does not expose an 'HLODLayer' UPROPERTY; assignment skipped."));
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT("hlod_layer_path"),
						ObjProp
							? TEXT("Actor HLODLayer readback did not match requested layer.")
							: TEXT("Actor class does not expose an 'HLODLayer' UPROPERTY."));
					Error = TEXT("HLODLayer assignment was not verified.");
					return false;
				}

				Summary = FString::Printf(TEXT("Actor '%s' HLOD layer set to '%s' (applied=%s)."),
					*Actor->GetName(),
					Layer ? *Layer->GetName() : TEXT("<cleared>"),
					bApplied ? TEXT("yes") : TEXT("no"));
				return true;
			},
			nullptr, 0
		});

		// ── 5. worldpartition_hlod_force_rebuild ──────────────────────
		R.Register({
			TEXT("worldpartition_hlod_force_rebuild"),
			TEXT("Force-rebuild HLOD actors for the world (optional cell_filter_box). "
			     "Dispatches the editor HLOD rebuild command and returns an async receipt. "
			     "Filtered cell rebuild requests fail closed because the console command "
			     "cannot prove partial-cell targeting."),
			SB::Object({
				{TEXT("world_path"),       SB::String(TEXT("Optional level path. Empty = current editor world."))},
				{TEXT("cell_filter_box"),  SB::Array(SB::Number(),
					TEXT("Optional [x_min, y_min, x_max, y_max] world coords. Omit to rebuild all."))}
			}, {}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString WorldPath; Args->TryGetStringField(TEXT("world_path"), WorldPath);
				UWorld* World = ResolveWorld(Ctx.Services, WorldPath, Error);
				const FString WorldName = World ? World->GetPathName() : TEXT("<no world>");

				// Surface filter box echo so caller can verify their input was parsed.
				TArray<TSharedPtr<FJsonValue>> EchoBox;
				const TArray<TSharedPtr<FJsonValue>>* BoxArr = nullptr;
				const bool bHasFilterBox = Args->TryGetArrayField(TEXT("cell_filter_box"), BoxArr) && BoxArr;
				if (bHasFilterBox)
				{
					for (const TSharedPtr<FJsonValue>& V : *BoxArr)
					{
						EchoBox.Add(MakeShared<FJsonValueNumber>(V.IsValid() ? V->AsNumber() : 0.0));
					}
				}
				if (!World)
				{
					SololmcpError::NotFound(Out, WorldPath);
					return false;
				}
				if (bHasFilterBox)
				{
					Out->SetStringField(TEXT("world"), WorldName);
					Out->SetArrayField(TEXT("cell_filter_box"), EchoBox);
					Out->SetStringField(TEXT("status"), TEXT("unsupported_filter_box"));
					Out->SetBoolField(TEXT("async"), false);
					SololmcpError::Set(Out, TEXT("UNSUPPORTED"), TEXT("cell_filter_box"),
						TEXT("cell_filter_box cannot be honored by the console rebuild command."));
					Error = TEXT("cell_filter_box is not supported for force rebuild.");
					return false;
				}

				// UE 5.7: WorldPartitionHLODUtilities builder API is editor-private.
				// Fallback: invoke the well-known console command 'wp.HLOD.RebuildHLODs'
				// via GEngine->Exec -works reliably in 5.7 and triggers async rebuild.
				const FString Cmd = TEXT("wp.HLOD.RebuildHLODs");
				TSharedRef<FJsonObject> Junk = MakeShared<FJsonObject>(); FString Js, Je;
				const bool bExecOk = Ctx.Services.ExecuteConsole(Cmd, Junk, Js, Je);
				if (!bExecOk || !Je.IsEmpty() || ConsoleTextLooksLikeError(Js))
				{
					Out->SetStringField(TEXT("world"), WorldName);
					Out->SetArrayField(TEXT("cell_filter_box"), EchoBox);
					Out->SetStringField(TEXT("status"), TEXT("exec_failed"));
					Out->SetBoolField(TEXT("async"), false);
					Out->SetStringField(TEXT("dispatch"), Cmd);
					Out->SetStringField(TEXT("console_stdout"), Js);
					Out->SetStringField(TEXT("console_stderr"), Je);
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT("dispatch"),
						TEXT("HLOD rebuild console dispatch failed."));
					Error = !Je.IsEmpty() ? Je : TEXT("HLOD rebuild console dispatch failed.");
					return false;
				}

				Out->SetStringField(TEXT("world"), WorldName);
				Out->SetArrayField(TEXT("cell_filter_box"), EchoBox);
				Out->SetNumberField(TEXT("cells_processed"), 0); // async -result not known synchronously
				Out->SetNumberField(TEXT("hlod_actors_created"), 0);
				Out->SetStringField(TEXT("status"), TEXT("triggered_async"));
				Out->SetBoolField(TEXT("async"), true);
				Out->SetBoolField(TEXT("completed"), false);
				Out->SetStringField(TEXT("dispatch"), Cmd);
				Out->SetStringField(TEXT("note"),
					TEXT("Rebuild runs asynchronously; cell_filter_box not respected by console command. "
					     "Monitor Output Log for progress; use python_exec for filter-aware rebuilds."));

				Summary = TEXT("HLOD rebuild dispatched (async; completion not verified).");
				return true;
			},
			nullptr, 0
		});
	}
} // namespace UE::SOMOLMCP

#undef LOCTEXT_NAMESPACE
