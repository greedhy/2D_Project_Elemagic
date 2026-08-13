// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpXRTemplateTools.cpp  (P4-3)
// ----------------------------------------------------------------------------
// Five VR/AR template setup tools for the SOMOLMCP UE plugin (xr_* prefix):
//
//   1. xr_setup_pawn              — Spawn a Pawn with VR-style camera setup.
//   2. xr_add_motion_controllers  — Attach Left/Right MotionControllerComponents.
//   3. xr_add_floor               — Drop a floor mesh + collision for safe play area.
//   4. xr_create_teleport_volume  — Tag an existing actor as a teleport target.
//   5. xr_status                  — Read HMD / XR system / controller state.
//
// Total: 5 tools registered via RegisterXRTemplateTools(FSololmcpToolRegistry&).
//
// HARD CONSTRAINTS honored:
//   - Does NOT modify SololmcpToolRegistry.{h,cpp}.
//   - Does NOT modify .Build.cs (audit only — see P4_Cloth_XR_DELIVERY.md).
//   - Does NOT modify any other .cpp file.
//
// Wire-up required (delivery md describes; harness operator must do this):
//   1. Add `void RegisterXRTemplateTools(FSololmcpToolRegistry&);` in SololmcpToolRegistry.h.
//   2. Call it in `FSololmcpToolRegistry::FSololmcpToolRegistry()` near the other
//      registrations.
//
// Module dependency notes (UE 5.7)
// --------------------------------
// * "HeadMountedDisplay" — IXRTrackingSystem, UHeadMountedDisplayFunctionLibrary.
// * "XRBase"             — UMotionControllerComponent (FName tracking source).
// * "InputCore"          — already in .Build.cs (FKey for default mappings).
// * If either is missing the file still compiles via __has_include guards but
//   tools degrade to NOT_AVAILABLE. See P4_Cloth_XR_DELIVERY.md for the exact
//   .Build.cs additions required for full functionality.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

// Engine
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/DefaultPawn.h"
#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Camera/CameraComponent.h"
#include "EngineUtils.h"

// Editor
#include "Editor.h"
#include "ScopedTransaction.h"

// Json
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// XR / HMD — probed because they live in modules that may not be in .Build.cs.
#if __has_include("MotionControllerComponent.h")
	#include "MotionControllerComponent.h"
	#define SOMOLMCP_HAS_MOTION_CONTROLLER 1
#else
	#define SOMOLMCP_HAS_MOTION_CONTROLLER 0
#endif

#if __has_include("HeadMountedDisplayFunctionLibrary.h")
	#include "HeadMountedDisplayFunctionLibrary.h"
	#define SOMOLMCP_HAS_HMD_FUNCLIB 1
#else
	#define SOMOLMCP_HAS_HMD_FUNCLIB 0
#endif

#if __has_include("IXRTrackingSystem.h")
	#include "IXRTrackingSystem.h"
	#define SOMOLMCP_HAS_XR_TRACKING 1
#else
	#define SOMOLMCP_HAS_XR_TRACKING 0
#endif

namespace UE::SOMOLMCP
{
namespace XRImpl
{
	// -----------------------------------------------------------------
	// Generic actor resolution helpers
	// -----------------------------------------------------------------
	static AActor* ResolveActorByLabel(UWorld* World, const FString& Id)
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

	static UWorld* GetEditorWorld(TSharedRef<FJsonObject>& OutStructured, FString& OutError)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("No editor world available."));
			OutError = TEXT("No editor world.");
		}
		return World;
	}

	// Make a unique actor label (avoid collisions in the level outliner).
	static FString MakeUniqueLabel(UWorld* World, const FString& Base)
	{
		FString Candidate = Base;
		int32 N = 1;
		while (ResolveActorByLabel(World, Candidate) != nullptr)
		{
			Candidate = FString::Printf(TEXT("%s_%02d"), *Base, ++N);
			if (N > 999) break;
		}
		return Candidate;
	}

	// Try to load engine cube as a fallback floor mesh.
	static UStaticMesh* LoadDefaultCube()
	{
		// /Engine/BasicShapes/Cube.Cube ships with every UE install.
		return Cast<UStaticMesh>(StaticLoadObject(
			UStaticMesh::StaticClass(), nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	}

	static UMaterialInterface* TryLoadMaterial(const FString& Path)
	{
		if (Path.IsEmpty()) return nullptr;
		return Cast<UMaterialInterface>(StaticLoadObject(
			UMaterialInterface::StaticClass(), nullptr, *Path));
	}

	static void SetStatus(TSharedRef<FJsonObject>& OutStructured, const TCHAR* Status)
	{
		OutStructured->SetStringField(TEXT("status"), Status);
	}

	static bool IsUsableComponent(const UActorComponent* Component)
	{
		return IsValid(Component) && Component->IsRegistered();
	}
}

// ============================================================================
// Tool 1: xr_setup_pawn
// ============================================================================

static void RegisterXRSetupPawn(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("xr_setup_pawn"),
		TEXT("Spawn a Pawn in the level with a head-tracked CameraComponent (VR-style). Optionally use an existing pawn class via pawn_path; otherwise spawn ADefaultPawn."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("pawn_path"),    FSololmcpSchemaBuilder::String(TEXT("Optional: Blueprint or class path to spawn (e.g. /Game/VR/BP_VRPawn)"))},
			{TEXT("actor_label"),  FSololmcpSchemaBuilder::String(TEXT("Outliner label, default 'VRPawn'"))},
			{TEXT("location_x"),   FSololmcpSchemaBuilder::Number(TEXT("Spawn X (default 0)"))},
			{TEXT("location_y"),   FSololmcpSchemaBuilder::Number(TEXT("Spawn Y (default 0)"))},
			{TEXT("location_z"),   FSololmcpSchemaBuilder::Number(TEXT("Spawn Z (default 100, eye height)"))}
		}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UWorld* World = XRImpl::GetEditorWorld(OutStructured, OutError);
			if (!World) return false;

			FString PawnPath, Label;
			Arguments->TryGetStringField(TEXT("pawn_path"),   PawnPath);
			Arguments->TryGetStringField(TEXT("actor_label"), Label);
			if (Label.IsEmpty()) Label = TEXT("VRPawn");

			const float LocX = Arguments->HasTypedField<EJson::Number>(TEXT("location_x"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("location_x"))) : 0.0f;
			const float LocY = Arguments->HasTypedField<EJson::Number>(TEXT("location_y"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("location_y"))) : 0.0f;
			const float LocZ = Arguments->HasTypedField<EJson::Number>(TEXT("location_z"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("location_z"))) : 100.0f;
			const FVector SpawnLoc(LocX, LocY, LocZ);

			// Resolve pawn class — prefer user-supplied class, fall back to ADefaultPawn.
			UClass* PawnClass = nullptr;
			if (!PawnPath.IsEmpty())
			{
				FString ClassErr;
				PawnClass = Context.Services.ResolveClass(PawnPath, ClassErr);
				if (!PawnClass)
				{
					SololmcpError::InvalidPath(OutStructured, PawnPath);
					OutError = FString::Printf(TEXT("pawn_path '%s' did not resolve to a class: %s"),
						*PawnPath, *ClassErr);
					return false;
				}
				if (!PawnClass->IsChildOf(APawn::StaticClass()))
				{
					SololmcpError::InvalidType(OutStructured, TEXT("pawn_path"), TEXT("APawn subclass"));
					OutError = TEXT("pawn_path must resolve to an APawn subclass.");
					return false;
				}
			}
			else
			{
				PawnClass = ADefaultPawn::StaticClass();
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "XRSetupPawn", "SOMOLMCP XR Setup Pawn"));

			FActorSpawnParameters SP;
			SP.bAllowDuringConstructionScript = true;
			APawn* Pawn = World->SpawnActor<APawn>(PawnClass, SpawnLoc, FRotator::ZeroRotator, SP);
			if (!Pawn)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("World->SpawnActor<APawn> returned null."));
				OutError = TEXT("Failed to spawn pawn.");
				return false;
			}
			Pawn->Modify();
			Pawn->SetActorLabel(XRImpl::MakeUniqueLabel(World, Label));
			const bool bPawnSpawned = IsValid(Pawn);

			// Track which components we add so the caller knows what was created.
			TArray<TSharedPtr<FJsonValue>> CompsJson;
			TArray<TSharedPtr<FJsonValue>> WarningsJson;

			// Ensure a SceneComponent root if the spawned class has none (rare for ADefaultPawn).
			USceneComponent* Root = Pawn->GetRootComponent();
			if (!Root)
			{
				Root = NewObject<USceneComponent>(Pawn, USceneComponent::StaticClass(),
					TEXT("XRRoot"), RF_Transactional);
				if (Root)
				{
					Root->RegisterComponent();
					Pawn->SetRootComponent(Root);
					CompsJson.Add(MakeShared<FJsonValueString>(TEXT("XRRoot(SceneComponent)")));
				}
			}
			const bool bRootReady = XRImpl::IsUsableComponent(Root) && Pawn->GetRootComponent() == Root;
			if (!bRootReady)
			{
				WarningsJson.Add(MakeShared<FJsonValueString>(TEXT("Root SceneComponent was not registered or assigned.")));
			}

			// Add a CameraComponent at eye height — this is what the HMD will drive.
			UCameraComponent* Camera = nullptr;
			{
				TArray<UCameraComponent*> ExistingCams;
				Pawn->GetComponents<UCameraComponent>(ExistingCams);
				if (ExistingCams.Num() > 0)
				{
					Camera = ExistingCams[0];
				}
				else
				{
					Camera = NewObject<UCameraComponent>(Pawn, UCameraComponent::StaticClass(),
						TEXT("VRCamera"), RF_Transactional);
					if (Camera && Root)
					{
						Camera->SetupAttachment(Root);
						Camera->RegisterComponent();
						CompsJson.Add(MakeShared<FJsonValueString>(TEXT("VRCamera(CameraComponent)")));
					}
				}
				if (Camera)
				{
					// VR-friendly defaults: lock-to-HMD on, FOV mid-wide.
					Camera->bLockToHmd = true;
					Camera->FieldOfView = 90.0f;
					// 170cm eye height relative to root if root is at 0.
					Camera->SetRelativeLocation(FVector(0.f, 0.f, 170.f));
				}
			}
			const bool bCameraReady = XRImpl::IsUsableComponent(Camera) && Camera->bLockToHmd;
			if (!bCameraReady)
			{
				WarningsJson.Add(MakeShared<FJsonValueString>(TEXT("VRCamera was not created, registered, or configured.")));
			}

			SololmcpWriteFlush::EnsureFlushed(Pawn);

			const bool bFullyApplied = bPawnSpawned && bRootReady && bCameraReady;
			XRImpl::SetStatus(OutStructured, bFullyApplied ? TEXT("success") : TEXT("partial_success"));
			OutStructured->SetStringField(TEXT("actor_label"), Pawn->GetActorLabel());
			OutStructured->SetStringField(TEXT("actor_name"),  Pawn->GetName());
			OutStructured->SetStringField(TEXT("pawn_path"),   PawnPath.IsEmpty() ? TEXT("ADefaultPawn") : PawnPath);
			OutStructured->SetStringField(TEXT("class"),       PawnClass->GetPathName());
			OutStructured->SetArrayField (TEXT("components"),  CompsJson);
			OutStructured->SetArrayField (TEXT("warnings"),    WarningsJson);

			TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
			LocObj->SetNumberField(TEXT("x"), LocX);
			LocObj->SetNumberField(TEXT("y"), LocY);
			LocObj->SetNumberField(TEXT("z"), LocZ);
			OutStructured->SetObjectField(TEXT("location"), LocObj);

			OutSummary = FString::Printf(TEXT("%s VR Pawn '%s' at (%.1f, %.1f, %.1f) with %d component(s)"),
				bFullyApplied ? TEXT("Spawned") : TEXT("Partially spawned"),
				*Pawn->GetActorLabel(), LocX, LocY, LocZ, CompsJson.Num());
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool 2: xr_add_motion_controllers
// ============================================================================

static void RegisterXRAddMotionControllers(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("xr_add_motion_controllers"),
		TEXT("Add Left + Right UMotionControllerComponent to a pawn, attached to its root, with default tracking source mappings."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("pawn_actor_name"), FSololmcpSchemaBuilder::String(TEXT("Pawn label / name / path in the level"))}
		}, {TEXT("pawn_actor_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString PawnName;
			if (!Arguments->TryGetStringField(TEXT("pawn_actor_name"), PawnName) || PawnName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("pawn_actor_name"));
				OutError = TEXT("Missing pawn_actor_name.");
				return false;
			}

			UWorld* World = XRImpl::GetEditorWorld(OutStructured, OutError);
			if (!World) return false;

			AActor* Actor = XRImpl::ResolveActorByLabel(World, PawnName);
			if (!Actor)
			{
				SololmcpError::NotFound(OutStructured, PawnName);
				OutError = FString::Printf(TEXT("Pawn '%s' not found."), *PawnName);
				return false;
			}

#if !SOMOLMCP_HAS_MOTION_CONTROLLER
			SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
				TEXT("MotionControllerComponent.h not found. Add 'XRBase' (UE 5.x) and 'HeadMountedDisplay' to .Build.cs."));
			OutError = TEXT("UMotionControllerComponent not available at compile time.");
			return false;
#else
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "XRAddMotionControllers",
				"SOMOLMCP XR Add Motion Controllers"));
			Actor->Modify();

			USceneComponent* Root = Actor->GetRootComponent();
			if (!Root)
			{
				Root = NewObject<USceneComponent>(Actor, USceneComponent::StaticClass(),
					TEXT("XRRoot"), RF_Transactional);
				if (Root)
				{
					Root->RegisterComponent();
					Actor->SetRootComponent(Root);
				}
			}
			if (!XRImpl::IsUsableComponent(Root) || Actor->GetRootComponent() != Root)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("Unable to create or resolve a registered root component for motion controllers."));
				OutError = TEXT("Motion controller root component is not available.");
				XRImpl::SetStatus(OutStructured, TEXT("failed"));
				return false;
			}

			auto AddOne = [&](const TCHAR* CompName, const FName& Source) -> UMotionControllerComponent*
			{
				// If a controller with this name already exists, reuse it (idempotent).
				TArray<UMotionControllerComponent*> Existing;
				Actor->GetComponents<UMotionControllerComponent>(Existing);
				for (UMotionControllerComponent* MC : Existing)
				{
					if (MC && MC->GetName() == FString(CompName))
					{
						MC->Modify();
						MC->SetTrackingMotionSource(Source);
						return MC;
					}
				}
				UMotionControllerComponent* MC = NewObject<UMotionControllerComponent>(
					Actor, UMotionControllerComponent::StaticClass(), CompName, RF_Transactional);
				if (!MC) return nullptr;
				MC->SetupAttachment(Root);
				MC->SetTrackingMotionSource(Source);
				// UE 5.7: bDisplayDeviceModel removed/renamed; controller mesh visualization
				// is now opt-in via the SetShowDeviceModel() API in UE 5.6+ if available.
				// We skip the toggle to keep the build green; users can enable in-editor.
				MC->RegisterComponent();
				return MC;
			};

			UMotionControllerComponent* Left  = AddOne(TEXT("MotionController_Left"),  FName(TEXT("Left")));
			UMotionControllerComponent* Right = AddOne(TEXT("MotionController_Right"), FName(TEXT("Right")));
			const bool bLeftApplied = XRImpl::IsUsableComponent(Left);
			const bool bRightApplied = XRImpl::IsUsableComponent(Right);
			if (!bLeftApplied && !bRightApplied)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("No motion controller components were created or registered."));
				OutError = TEXT("Failed to add motion controller components.");
				XRImpl::SetStatus(OutStructured, TEXT("failed"));
				return false;
			}

			SololmcpWriteFlush::EnsureFlushed(Actor);

			XRImpl::SetStatus(OutStructured, (bLeftApplied && bRightApplied) ? TEXT("success") : TEXT("partial_success"));
			OutStructured->SetStringField(TEXT("actor"),
				Actor->GetActorLabel());
			OutStructured->SetStringField(TEXT("left_controller"),
				Left  ? Left ->GetName() : TEXT(""));
			OutStructured->SetStringField(TEXT("right_controller"),
				Right ? Right->GetName() : TEXT(""));
			OutStructured->SetBoolField(TEXT("left_applied"), bLeftApplied);
			OutStructured->SetBoolField(TEXT("right_applied"), bRightApplied);
			OutStructured->SetStringField(TEXT("left_tracking_source"),  TEXT("Left"));
			OutStructured->SetStringField(TEXT("right_tracking_source"), TEXT("Right"));

			OutSummary = FString::Printf(TEXT("%s motion controllers to '%s' (left=%s right=%s)"),
				(bLeftApplied && bRightApplied) ? TEXT("Added L/R") : TEXT("Partially added"),
				*Actor->GetActorLabel(),
				bLeftApplied ? TEXT("ok") : TEXT("failed"),
				bRightApplied ? TEXT("ok") : TEXT("failed"));
			return true;
#endif
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool 3: xr_add_floor
// ============================================================================

static void RegisterXRAddFloor(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("xr_add_floor"),
		TEXT("Spawn a flat floor StaticMeshActor centered at origin (size_cm x size_cm), with collision enabled — for VR safe play area."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("size_cm"),       FSololmcpSchemaBuilder::Number(TEXT("Square edge length in cm (default 5000 = 50m)"))},
			{TEXT("material_path"), FSololmcpSchemaBuilder::String(TEXT("Optional UMaterialInterface path to apply"))},
			{TEXT("actor_label"),   FSololmcpSchemaBuilder::String(TEXT("Outliner label (default 'XR_Floor')"))}
		}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UWorld* World = XRImpl::GetEditorWorld(OutStructured, OutError);
			if (!World) return false;

			const float Size = Arguments->HasTypedField<EJson::Number>(TEXT("size_cm"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("size_cm"))) : 5000.0f;
			FString MatPath, Label;
			Arguments->TryGetStringField(TEXT("material_path"), MatPath);
			Arguments->TryGetStringField(TEXT("actor_label"),   Label);
			if (Label.IsEmpty()) Label = TEXT("XR_Floor");

			UStaticMesh* CubeMesh = XRImpl::LoadDefaultCube();
			if (!CubeMesh)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("/Engine/BasicShapes/Cube.Cube failed to load."));
				OutError = TEXT("Engine cube mesh unavailable.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "XRAddFloor", "SOMOLMCP XR Add Floor"));

			FActorSpawnParameters SP;
			SP.bAllowDuringConstructionScript = true;
			AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
				AStaticMeshActor::StaticClass(),
				FVector(0, 0, -5.0f),  // sit slightly below origin so a Z=0 pawn stands on it.
				FRotator::ZeroRotator, SP);
			if (!Floor)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("SpawnActor<AStaticMeshActor> returned null."));
				OutError = TEXT("Failed to spawn floor actor.");
				return false;
			}
			Floor->Modify();
			Floor->SetMobility(EComponentMobility::Static);
			Floor->SetActorLabel(XRImpl::MakeUniqueLabel(World, Label));

			UStaticMeshComponent* SMC = Floor->GetStaticMeshComponent();
			bool bMeshApplied = false;
			bool bCollisionApplied = false;
			bool bMaterialApplied = false;
			const bool bMaterialRequested = !MatPath.IsEmpty();
			if (SMC)
			{
				SMC->Modify();
				SMC->SetStaticMesh(CubeMesh);
				// Engine cube is 100x100x100cm — scale to (Size/100) on X/Y, thin slab on Z.
				const float HorizScale = FMath::Max(Size, 100.0f) / 100.0f;
				SMC->SetWorldScale3D(FVector(HorizScale, HorizScale, 0.1f));
				SMC->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				SMC->SetCollisionProfileName(TEXT("BlockAll"));
				if (UMaterialInterface* Mat = XRImpl::TryLoadMaterial(MatPath))
				{
					SMC->SetMaterial(0, Mat);
					bMaterialApplied = SMC->GetMaterial(0) == Mat;
				}
				bMeshApplied = SMC->GetStaticMesh() == CubeMesh;
				bCollisionApplied = SMC->GetCollisionEnabled() != ECollisionEnabled::NoCollision;
			}

			SololmcpWriteFlush::EnsureFlushed(Floor);
			const bool bFullyApplied = IsValid(Floor) && bMeshApplied && bCollisionApplied && (!bMaterialRequested || bMaterialApplied);
			XRImpl::SetStatus(OutStructured, bFullyApplied ? TEXT("success") : TEXT("partial_success"));

			OutStructured->SetStringField(TEXT("actor_label"), Floor->GetActorLabel());
			OutStructured->SetStringField(TEXT("actor_name"),  Floor->GetName());
			OutStructured->SetNumberField(TEXT("size"),        Size);
			OutStructured->SetBoolField(TEXT("mesh_applied"), bMeshApplied);
			OutStructured->SetBoolField(TEXT("collision_applied"), bCollisionApplied);
			TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
			const FVector L = Floor->GetActorLocation();
			LocObj->SetNumberField(TEXT("x"), L.X);
			LocObj->SetNumberField(TEXT("y"), L.Y);
			LocObj->SetNumberField(TEXT("z"), L.Z);
			OutStructured->SetObjectField(TEXT("location"), LocObj);
			OutStructured->SetBoolField(TEXT("material_applied"), bMaterialApplied);
			OutStructured->SetStringField(TEXT("material_path"),
				MatPath.IsEmpty() ? TEXT("(default)") : MatPath);

			OutSummary = FString::Printf(TEXT("%s VR floor '%s' (%.0f cm, mesh=%s collision=%s material=%s)"),
				bFullyApplied ? TEXT("Spawned") : TEXT("Partially spawned"),
				*Floor->GetActorLabel(), Size,
				bMeshApplied ? TEXT("ok") : TEXT("failed"),
				bCollisionApplied ? TEXT("ok") : TEXT("failed"),
				(!bMaterialRequested || bMaterialApplied) ? TEXT("ok") : TEXT("failed"));
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool 4: xr_create_teleport_volume
// ============================================================================

static void RegisterXRCreateTeleportVolume(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("xr_create_teleport_volume"),
		TEXT("Mark an existing actor as a teleport target: adds a tag (default 'TeleportTarget') and forces a navigable collision profile."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("Existing actor label / name / path"))},
			{TEXT("tag"),        FSololmcpSchemaBuilder::String(TEXT("Tag to add (default 'TeleportTarget')"))}
		}, {TEXT("actor_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString ActorName, Tag;
			if (!Arguments->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("actor_name"));
				OutError = TEXT("Missing actor_name.");
				return false;
			}
			Arguments->TryGetStringField(TEXT("tag"), Tag);
			if (Tag.IsEmpty()) Tag = TEXT("TeleportTarget");

			UWorld* World = XRImpl::GetEditorWorld(OutStructured, OutError);
			if (!World) return false;

			AActor* Actor = XRImpl::ResolveActorByLabel(World, ActorName);
			if (!Actor)
			{
				SololmcpError::NotFound(OutStructured, ActorName);
				OutError = FString::Printf(TEXT("Actor '%s' not found."), *ActorName);
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "XRCreateTeleportVolume",
				"SOMOLMCP XR Create Teleport Volume"));
			Actor->Modify();

			TArray<TSharedPtr<FJsonValue>> AddedTags;
			const FName TagName(*Tag);
			if (!Actor->Tags.Contains(TagName))
			{
				Actor->Tags.Add(TagName);
				AddedTags.Add(MakeShared<FJsonValueString>(Tag));
			}

			// Apply a teleport-friendly collision profile to all primitive components.
			// "BlockAll" + visibility-trace block is what the standard VR template
			// uses for arc-trace targeting.
			TArray<UPrimitiveComponent*> Prims;
			Actor->GetComponents<UPrimitiveComponent>(Prims);
			int32 TouchedComps = 0;
			int32 VerifiedComps = 0;
			for (UPrimitiveComponent* P : Prims)
			{
				if (!P) continue;
				P->Modify();
				P->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
				P->SetCollisionResponseToChannel(ECC_Camera,     ECR_Block);
				++TouchedComps;
				if (P->GetCollisionResponseToChannel(ECC_Visibility) == ECR_Block
					&& P->GetCollisionResponseToChannel(ECC_Camera) == ECR_Block)
				{
					++VerifiedComps;
				}
			}
			const bool bTagApplied = Actor->Tags.Contains(TagName);
			const bool bHadPrimitiveWork = Prims.Num() > 0;
			const bool bCollisionFullyApplied = !bHadPrimitiveWork || (VerifiedComps == TouchedComps && TouchedComps > 0);
			if (!bTagApplied && TouchedComps == 0)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("Teleport tag was not applied and no primitive components were updated."));
				OutError = TEXT("No teleport-volume changes were applied.");
				XRImpl::SetStatus(OutStructured, TEXT("failed"));
				return false;
			}

			SololmcpWriteFlush::EnsureFlushed(Actor);

			XRImpl::SetStatus(OutStructured, (bTagApplied && bCollisionFullyApplied) ? TEXT("success") : TEXT("partial_success"));
			OutStructured->SetStringField(TEXT("actor"),       Actor->GetActorLabel());
			OutStructured->SetStringField(TEXT("actor_name"),  Actor->GetName());
			OutStructured->SetArrayField (TEXT("tags_added"),  AddedTags);
			OutStructured->SetNumberField(TEXT("primitives_updated"), TouchedComps);
			OutStructured->SetNumberField(TEXT("primitives_verified"), VerifiedComps);
			OutStructured->SetBoolField(TEXT("tag_applied"), bTagApplied);

			OutSummary = FString::Printf(TEXT("%s '%s' as teleport target (tag=%s, comps=%d/%d)"),
				(bTagApplied && bCollisionFullyApplied) ? TEXT("Tagged") : TEXT("Partially tagged"),
				*Actor->GetActorLabel(),
				bTagApplied ? TEXT("ok") : TEXT("failed"),
				VerifiedComps, TouchedComps);
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool 5: xr_status
// ============================================================================

static void RegisterXRStatus(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("xr_status"),
		TEXT("Diagnostic snapshot: HMD connectivity, XR system name, basic L/R controller state."),
		FSololmcpSchemaBuilder::Object({} /* no args */),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			bool bHmdConnected   = false;
			bool bHmdEnabled     = false;
			FString HmdName      = TEXT("");
			FString XrSystemName = TEXT("None");

#if SOMOLMCP_HAS_HMD_FUNCLIB
			bHmdConnected = UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayConnected();
			bHmdEnabled   = UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled();
			HmdName       = UHeadMountedDisplayFunctionLibrary::GetHMDDeviceName().ToString();
#endif

#if SOMOLMCP_HAS_XR_TRACKING
			if (GEngine && GEngine->XRSystem.IsValid())
			{
				XrSystemName = GEngine->XRSystem->GetSystemName().ToString();
				if (HmdName.IsEmpty())
				{
					HmdName = XrSystemName;
				}
			}
#endif

			// L/R controller probe — strings are descriptive only because tracking
			// state requires an active PIE/standalone session; in editor we just
			// report whether MotionController support is compiled in.
			TSharedRef<FJsonObject> LeftJson  = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> RightJson = MakeShared<FJsonObject>();
		#if SOMOLMCP_HAS_MOTION_CONTROLLER
			LeftJson ->SetBoolField(TEXT("supported"), true);
			RightJson->SetBoolField(TEXT("supported"), true);
			LeftJson ->SetStringField(TEXT("source"), TEXT("Left"));
			RightJson->SetStringField(TEXT("source"), TEXT("Right"));
			LeftJson ->SetStringField(TEXT("state"),  bHmdConnected ? TEXT("ready") : TEXT("no_hmd"));
			RightJson->SetStringField(TEXT("state"),  bHmdConnected ? TEXT("ready") : TEXT("no_hmd"));
		#else
			LeftJson ->SetBoolField(TEXT("supported"), false);
			RightJson->SetBoolField(TEXT("supported"), false);
			LeftJson ->SetStringField(TEXT("state"), TEXT("module_missing"));
			RightJson->SetStringField(TEXT("state"), TEXT("module_missing"));
		#endif

			OutStructured->SetBoolField  (TEXT("hmd_connected"), bHmdConnected);
			OutStructured->SetBoolField  (TEXT("hmd_enabled"),   bHmdEnabled);
			OutStructured->SetStringField(TEXT("hmd_name"),      HmdName);
			OutStructured->SetStringField(TEXT("xr_system"),     XrSystemName);
			OutStructured->SetObjectField(TEXT("left_controller_state"),  LeftJson);
			OutStructured->SetObjectField(TEXT("right_controller_state"), RightJson);

			OutSummary = FString::Printf(TEXT("XR: hmd=%s connected=%s sys=%s"),
				HmdName.IsEmpty() ? TEXT("(none)") : *HmdName,
				bHmdConnected ? TEXT("yes") : TEXT("no"),
				*XrSystemName);
			return true;
		}
	, nullptr
	, 2
	});
}

// ============================================================================
// Public registration
// ============================================================================

void RegisterXRTemplateTools(FSololmcpToolRegistry& Registry)
{
	RegisterXRSetupPawn             (Registry);
	RegisterXRAddMotionControllers  (Registry);
	RegisterXRAddFloor              (Registry);
	RegisterXRCreateTeleportVolume  (Registry);
	RegisterXRStatus                (Registry);
}

} // namespace UE::SOMOLMCP
