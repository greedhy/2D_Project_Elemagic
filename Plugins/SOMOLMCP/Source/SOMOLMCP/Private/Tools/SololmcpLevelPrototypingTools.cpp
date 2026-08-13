// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpLevelPrototypingTools.cpp — SOMOLMCP v3.5.0
// Level Design & Prototyping Automation Tools

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpJsonUtils.h"

// ── UE Editor ──
#include "Editor.h"
#include "Subsystems/UnrealEditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPLevelPrototyping, Log, All);

namespace UE::SOMOLMCP
{

static bool EnsureGameThread(FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("Level Prototyping tools must be called from the game thread.");
		return false;
	}
	return true;
}

static int32 CountActorsWithLabel(UWorld* World, const FString& Label)
{
	if (!World) return 0;
	int32 Count = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetActorLabel() == Label)
		{
			++Count;
		}
	}
	return Count;
}

void RegisterLevelPrototypingTools(FSololmcpToolRegistry& Registry)
{
	// ─────────────────────────────────────────────────────────────────────────
	// 1. level_setup_daynight_rig
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("level_setup_daynight_rig"),
		TEXT("Automatically create and configure a standard UE5 physical sky and day-night lighting rig. "
			 "Spawns DirectionalLight, SkyLight, SkyAtmosphere, VolumetricCloud, and ExponentialHeightFog, and links them properly for Physically Based Rendering."),
		FSololmcpSchemaBuilder::Object({}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("No editor world."); return false; }

			// Use Python to safely spawn and configure actors to avoid missing C++ header/module dependencies for SkyAtmosphere etc.
			// The rig is intentionally idempotent: re-running it must not create
			// competing DirectionalLights. UE forward shading / translucent water /
			// fog paths expect one dominant sun, and previous revisions spawned a
			// new low-intensity sun every time, leaving scenes visually black.
			FString PythonCode = R"(
import unreal

MANAGED = {
    'sun': 'SOM_Daylight_Sun',
    'sky': 'SOM_Daylight_Sky',
    'atmosphere': 'SOM_SkyAtmosphere',
    'cloud': 'SOM_VolumetricCloud',
    'fog': 'SOM_HeightFog',
    'post': 'SOM_GlobalPostProcess',
}

def set_prop(obj, name, value):
    try:
        obj.set_editor_property(name, value)
        return True
    except Exception:
        return False

def find_by_label(label):
    for actor in unreal.EditorLevelLibrary.get_all_level_actors():
        try:
            if actor.get_actor_label() == label:
                return actor
        except Exception:
            pass
    return None

def find_first_actor_with_component(component_cls):
    for actor in unreal.EditorLevelLibrary.get_all_level_actors():
        if get_component(actor, component_cls):
            return actor
    return None

def ensure_actor(cls, label, location):
    actor = find_by_label(label)
    if actor is None and cls == unreal.DirectionalLight:
        actor = find_first_actor_with_component(unreal.DirectionalLightComponent)
    if actor is None:
        actor = unreal.EditorLevelLibrary.spawn_actor_from_class(cls, location)
    if actor:
        actor.set_actor_label(label)
    return actor

def get_component(actor, component_cls):
    if not actor:
        return None
    try:
        return actor.get_component_by_class(component_cls)
    except Exception:
        return None

def demote_duplicate_directionals(primary):
    destroyed = 0
    for actor in list(unreal.EditorLevelLibrary.get_all_level_actors()):
        if actor == primary:
            continue
        comp = get_component(actor, unreal.DirectionalLightComponent)
        if not comp:
            continue
        try:
            unreal.EditorLevelLibrary.destroy_actor(actor)
            destroyed += 1
        except Exception:
            set_prop(comp, 'atmosphere_sun_light', False)
            set_prop(comp, 'intensity', 0.0)
    return destroyed

def demote_duplicate_skylights(primary):
    for actor in unreal.EditorLevelLibrary.get_all_level_actors():
        if actor == primary:
            continue
        comp = get_component(actor, unreal.SkyLightComponent)
        if not comp:
            continue
        label = actor.get_actor_label()
        if label.startswith('SkyLight') and label != MANAGED['sky']:
            set_prop(comp, 'intensity', 0.0)
            set_prop(comp, 'real_time_capture', False)

def setup_lighting():
    world = unreal.EditorLevelLibrary.get_editor_world()
    if not world: return False

    # Single managed primary sun. Outdoor daylight uses physical lux scale.
    dir_light = ensure_actor(unreal.DirectionalLight, MANAGED['sun'], unreal.Vector(0,0,500))
    if dir_light:
        dir_light.set_actor_rotation(unreal.Rotator(-35, 135, 0), False)
        comp = dir_light.get_component_by_class(unreal.DirectionalLightComponent)
        if comp:
            comp.set_editor_property('mobility', unreal.ComponentMobility.MOVABLE)
            comp.set_editor_property('atmosphere_sun_light_index', 0)
            comp.set_editor_property('atmosphere_sun_light', True)
            comp.set_editor_property('cast_volumetric_shadow', True)
            comp.set_editor_property('use_temperature', True)
            comp.set_editor_property('temperature', 5400.0)
            comp.set_editor_property('intensity', 80000.0)
            set_prop(comp, 'forward_shading_priority', 1)
    destroyed_directionals = demote_duplicate_directionals(dir_light)
    try:
        unreal.SystemLibrary.execute_console_command(world, 'r.EyeAdaptation.CachedLightingPreExposure -10')
    except Exception:
        pass

    # Managed SkyLight.
    sky_light = ensure_actor(unreal.SkyLight, MANAGED['sky'], unreal.Vector(0,0,400))
    if sky_light:
        comp = sky_light.get_component_by_class(unreal.SkyLightComponent)
        if comp:
            comp.set_editor_property('mobility', unreal.ComponentMobility.MOVABLE)
            comp.set_editor_property('real_time_capture', True)
            comp.set_editor_property('intensity', 1.5)
    demote_duplicate_skylights(sky_light)

    # Managed atmosphere/cloud/fog.
    sky_atmos = ensure_actor(unreal.SkyAtmosphere, MANAGED['atmosphere'], unreal.Vector(0,0,300))

    cloud = ensure_actor(unreal.VolumetricCloud, MANAGED['cloud'], unreal.Vector(0,0,200))

    fog = ensure_actor(unreal.ExponentialHeightFog, MANAGED['fog'], unreal.Vector(0,0,100))
    if fog:
        comp = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
        if comp:
            comp.set_editor_property('volumetric_fog', True)
            comp.set_editor_property('fog_density', 0.004)
            comp.set_editor_property('fog_height_falloff', 0.18)

    pp = ensure_actor(unreal.PostProcessVolume, MANAGED['post'], unreal.Vector(0,0,0))
    if pp:
        set_prop(pp, 'unbound', True)
        set_prop(pp, 'priority', 10.0)

    return True

result_ok = setup_lighting()
print(f"LIGHTING_SETUP_RESULT:{result_ok}")
)";
			TSharedRef<FJsonObject> PythonResult = MakeShared<FJsonObject>();
			FString PythonSummary, PythonErr;
			bool bOk = Context.Services.ExecutePython(PythonCode, TEXT("ExecuteFile"), true, PythonResult, PythonSummary, PythonErr);

			OutStructured->SetBoolField(TEXT("python_success"), bOk);
			OutStructured->SetStringField(TEXT("python_output"), PythonSummary);
			
			const bool bScriptReportedOk = PythonSummary.Contains(TEXT("LIGHTING_SETUP_RESULT:True"))
				|| PythonSummary.Contains(TEXT("LIGHTING_SETUP_RESULT: true"))
				|| PythonSummary.Contains(TEXT("LIGHTING_SETUP_RESULT:1"));
			const int32 VerifiedActors =
				(CountActorsWithLabel(World, TEXT("SOM_Daylight_Sun")) > 0 ? 1 : 0) +
				(CountActorsWithLabel(World, TEXT("SOM_Daylight_Sky")) > 0 ? 1 : 0) +
				(CountActorsWithLabel(World, TEXT("SOM_SkyAtmosphere")) > 0 ? 1 : 0) +
				(CountActorsWithLabel(World, TEXT("SOM_VolumetricCloud")) > 0 ? 1 : 0) +
				(CountActorsWithLabel(World, TEXT("SOM_HeightFog")) > 0 ? 1 : 0) +
				(CountActorsWithLabel(World, TEXT("SOM_GlobalPostProcess")) > 0 ? 1 : 0);
			OutStructured->SetNumberField(TEXT("verified_actor_count"), VerifiedActors);

			if (bOk && bScriptReportedOk && VerifiedActors == 6)
			{
				OutSummary = TEXT("Successfully ensured UE5 daylight rig (single Sun, SkyLight, Atmosphere, Clouds, Fog, PostProcess).");
				return true;
			}
			else
			{
				OutError = FString::Printf(TEXT("Lighting setup did not verify after Python execution (python_ok=%s, reported_ok=%s, verified=%d/5, error='%s')."),
					bOk ? TEXT("true") : TEXT("false"),
					bScriptReportedOk ? TEXT("true") : TEXT("false"),
					VerifiedActors,
					*PythonErr);
				return false;
			}
		},
	nullptr,
	0,
	nullptr,
	true
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 2. level_spawn_blockout
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("level_spawn_blockout"),
		TEXT("Spawn a primitive greyboxing shape (Cube, Sphere, Cylinder, Plane, Cone) with specific absolute dimensions (in cm). "
			 "Automatically snaps to the ground if snap_to_ground is true."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("shape_type"), FSololmcpSchemaBuilder::String(TEXT("Primitive type. Options: Cube | Sphere | Cylinder | Plane | Cone"),
					{TEXT("Cube"), TEXT("Sphere"), TEXT("Cylinder"), TEXT("Plane"), TEXT("Cone")})},
				{TEXT("size_x"), FSololmcpSchemaBuilder::Number(TEXT("Absolute size in X dimension (cm). Default 100."))},
				{TEXT("size_y"), FSololmcpSchemaBuilder::Number(TEXT("Absolute size in Y dimension (cm). Default 100."))},
				{TEXT("size_z"), FSololmcpSchemaBuilder::Number(TEXT("Absolute size in Z dimension (cm). Default 100."))},
				{TEXT("location_x"), FSololmcpSchemaBuilder::Number(TEXT("Spawn X position. Default 0."))},
				{TEXT("location_y"), FSololmcpSchemaBuilder::Number(TEXT("Spawn Y position. Default 0."))},
				{TEXT("location_z"), FSololmcpSchemaBuilder::Number(TEXT("Spawn Z position. Default 100."))},
				{TEXT("rotation_yaw"), FSololmcpSchemaBuilder::Number(TEXT("Yaw rotation in degrees. Default 0."))},
				{TEXT("snap_to_ground"), FSololmcpSchemaBuilder::Boolean(TEXT("If true, line trace downwards to snap actor to floor. Default true."))},
				{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Optional actor label."))}
			}, {TEXT("shape_type")}
		),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			UWorld* World = nullptr;
			if (GEditor) World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			FString ShapeType;
			Arguments->TryGetStringField(TEXT("shape_type"), ShapeType);
			double SizeX=100.0, SizeY=100.0, SizeZ=100.0;
			Arguments->TryGetNumberField(TEXT("size_x"), SizeX);
			Arguments->TryGetNumberField(TEXT("size_y"), SizeY);
			Arguments->TryGetNumberField(TEXT("size_z"), SizeZ);
			double LocX=0.0, LocY=0.0, LocZ=100.0;
			Arguments->TryGetNumberField(TEXT("location_x"), LocX);
			Arguments->TryGetNumberField(TEXT("location_y"), LocY);
			Arguments->TryGetNumberField(TEXT("location_z"), LocZ);
			double RotationYaw = 0.0;
			Arguments->TryGetNumberField(TEXT("rotation_yaw"), RotationYaw);
			bool bSnap = true;
			Arguments->TryGetBoolField(TEXT("snap_to_ground"), bSnap);
			FString ActorLabel;
			Arguments->TryGetStringField(TEXT("label"), ActorLabel);

			TArray<FString> MeshCandidates;
			if (ShapeType.Equals(TEXT("Sphere"), ESearchCase::IgnoreCase))
			{
				MeshCandidates = { TEXT("/Engine/BasicShapes/Sphere.Sphere") };
				ShapeType = TEXT("Sphere");
			}
			else if (ShapeType.Equals(TEXT("Cylinder"), ESearchCase::IgnoreCase))
			{
				MeshCandidates = { TEXT("/Engine/BasicShapes/Cylinder.Cylinder") };
				ShapeType = TEXT("Cylinder");
			}
			else if (ShapeType.Equals(TEXT("Plane"), ESearchCase::IgnoreCase))
			{
				MeshCandidates = { TEXT("/Engine/BasicShapes/Plane.Plane") };
				ShapeType = TEXT("Plane");
			}
			else if (ShapeType.Equals(TEXT("Cone"), ESearchCase::IgnoreCase))
			{
				MeshCandidates = { TEXT("/Engine/BasicShapes/Cone.Cone") };
				ShapeType = TEXT("Cone");
			}
			else
			{
				// UE 5.7 SOMOL installs can have Cube1/Cube_01 while UE 5.8 has Cube.
				MeshCandidates = {
					TEXT("/Engine/BasicShapes/Cube.Cube"),
					TEXT("/Engine/BasicShapes/Cube1.Cube1"),
					TEXT("/Engine/BasicShapes/Cube_01.Cube_01"),
					TEXT("/Engine/EngineMeshes/Cube.Cube"),
					TEXT("/Engine/EditorMeshes/EditorCube.EditorCube")
				};
				ShapeType = TEXT("Cube");
			}

			FString MeshPath;
			UStaticMesh* MeshAsset = nullptr;
			for (const FString& CandidatePath : MeshCandidates)
			{
				MeshAsset = LoadObject<UStaticMesh>(nullptr, *CandidatePath);
				if (MeshAsset)
				{
					MeshPath = CandidatePath;
					break;
				}
			}
			if (!MeshAsset)
			{
				OutError = FString::Printf(TEXT("Failed to load basic shape mesh for '%s'. Tried: %s"),
					*ShapeType, *FString::Join(MeshCandidates, TEXT(", ")));
				return false;
			}

			FVector BaseScale(SizeX/100.0, SizeY/100.0, SizeZ/100.0);
			FVector Location(LocX, LocY, LocZ);

			if (bSnap)
			{
				FHitResult HitResult;
				// SNAP-CHANNEL fix (2026-06-09): standardize on ECC_WorldStatic (ground/
				// landscape/static geometry) — matches the world_create placement path
				// (SololmcpWorldCreateTools.cpp). ECC_Visibility was inconsistent and could
				// snap to non-ground visibility-only geometry. Widen the trace to ±2000m
				// (was ±500m) so tall centered terrain (max_elevation up to 500m) is reached.
				FVector Start = Location + FVector(0, 0, 200000.0);
				FVector End = Location - FVector(0, 0, 200000.0);
				FCollisionQueryParams Params(SCENE_QUERY_STAT(SnapToGround), true);
				if (World->LineTraceSingleByChannel(HitResult, Start, End, ECC_WorldStatic, Params))
				{
					// Snap bottom of mesh to hit location
					// Cube is centered, so half height needs to be added
					double HalfHeight = (100.0 * BaseScale.Z) / 2.0;
					if (ShapeType == TEXT("Plane")) HalfHeight = 0.0; // plane is flat
					Location = HitResult.Location + FVector(0, 0, HalfHeight);
				}
			}

			const FRotator SpawnRotation(0.0, RotationYaw, 0.0);
			AStaticMeshActor* NewActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Location, SpawnRotation);
			if (!NewActor || !NewActor->GetStaticMeshComponent())
			{
				OutError = TEXT("Failed to spawn StaticMeshActor.");
				return false;
			}

			NewActor->GetStaticMeshComponent()->SetStaticMesh(MeshAsset);
			NewActor->SetActorScale3D(BaseScale);

			if (!ActorLabel.IsEmpty())
			{
				NewActor->SetActorLabel(ActorLabel);
			}
			else
			{
				NewActor->SetActorLabel(FString::Printf(TEXT("Blockout_%s"), *ShapeType));
			}

			if (NewActor->GetStaticMeshComponent()->GetStaticMesh() != MeshAsset)
			{
				OutError = FString::Printf(TEXT("Spawned blockout actor '%s' but StaticMesh readback did not match '%s'."),
					*NewActor->GetName(), *MeshPath);
				return false;
			}

			OutStructured->SetStringField(TEXT("actor_label"), NewActor->GetActorLabel());
			OutStructured->SetStringField(TEXT("actor_name"), NewActor->GetName());
			OutStructured->SetStringField(TEXT("mesh_path"), MeshPath);
			OutStructured->SetNumberField(TEXT("location_x"), Location.X);
			OutStructured->SetNumberField(TEXT("location_y"), Location.Y);
			OutStructured->SetNumberField(TEXT("location_z"), Location.Z);
			OutStructured->SetNumberField(TEXT("rotation_yaw"), NewActor->GetActorRotation().Yaw);
			OutStructured->SetNumberField(TEXT("size_x"), SizeX);
			OutStructured->SetNumberField(TEXT("size_y"), SizeY);
			OutStructured->SetNumberField(TEXT("size_z"), SizeZ);
			OutStructured->SetNumberField(TEXT("scale_x"), BaseScale.X);
			OutStructured->SetNumberField(TEXT("scale_y"), BaseScale.Y);
			OutStructured->SetNumberField(TEXT("scale_z"), BaseScale.Z);
			OutStructured->SetBoolField(TEXT("snap_to_ground"), bSnap);
			OutSummary = FString::Printf(TEXT("Spawned blockout shape %s '%s' at (%.2f, %.2f, %.2f)"), 
				*ShapeType, *NewActor->GetActorLabel(), Location.X, Location.Y, Location.Z);
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 3. level_scatter_on_surface
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("level_scatter_on_surface"),
		TEXT("Procedurally scatter instances of an actor/mesh on the surface within a specified bounding box. "
			 "Uses downward raycasting to find the floor/terrain and aligns the spawned actors with the surface normal."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Path to StaticMesh or Blueprint asset. e.g. /Game/Props/SM_Tree"))},
				{TEXT("count"), FSololmcpSchemaBuilder::Integer(TEXT("Number of instances to spawn."))},
				{TEXT("center_x"), FSololmcpSchemaBuilder::Number(TEXT("Center X of scattering area."))},
				{TEXT("center_y"), FSololmcpSchemaBuilder::Number(TEXT("Center Y of scattering area."))},
				{TEXT("extent_x"), FSololmcpSchemaBuilder::Number(TEXT("Radius/Extent X of scattering area."))},
				{TEXT("extent_y"), FSololmcpSchemaBuilder::Number(TEXT("Radius/Extent Y of scattering area."))},
				{TEXT("align_to_normal"), FSololmcpSchemaBuilder::Boolean(TEXT("Whether to align actors to surface normal. Default False (upright)."))},
				{TEXT("random_scale_min"), FSololmcpSchemaBuilder::Number(TEXT("Minimum random uniform scale. Default 0.8."))},
				{TEXT("random_scale_max"), FSololmcpSchemaBuilder::Number(TEXT("Maximum random uniform scale. Default 1.2."))}
			}, {TEXT("asset_path"), TEXT("count"), TEXT("center_x"), TEXT("center_y"), TEXT("extent_x"), TEXT("extent_y")}
		),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			UWorld* World = nullptr;
			if (GEditor) World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			FString AssetPath; Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
			int32 Count=0; Arguments->TryGetNumberField(TEXT("count"), Count);
			double CX=0, CY=0, EX=1000, EY=1000;
			Arguments->TryGetNumberField(TEXT("center_x"), CX);
			Arguments->TryGetNumberField(TEXT("center_y"), CY);
			Arguments->TryGetNumberField(TEXT("extent_x"), EX);
			Arguments->TryGetNumberField(TEXT("extent_y"), EY);
			bool bAlign=false; Arguments->TryGetBoolField(TEXT("align_to_normal"), bAlign);
			double ScaleMin=0.8, ScaleMax=1.2;
			Arguments->TryGetNumberField(TEXT("random_scale_min"), ScaleMin);
			Arguments->TryGetNumberField(TEXT("random_scale_max"), ScaleMax);

			// FIX (v12-rd2): hardcoded cap raised 1000 -> 50000.
			// Original cap was overly conservative; the spawn loop has no per-iter
			// transaction cost (verified by code inspection) so 50000 instances per
			// call is safe. For dense PCG/foliage use cases callers commonly want
			// 5000-30000 per call.
			if (Count <= 0 || Count > 50000)
			{
				OutError = TEXT("Count must be between 1 and 50000.");
				return false;
			}

			// We use Python to do the asset loading + spawning to easily handle both Blueprints and StaticMeshes
			FString PythonCode;
			PythonCode += TEXT("import unreal\n");
			PythonCode += TEXT("import random\n");
			// Helper to get rotation from Z normal
			PythonCode += TEXT("def rotation_from_z(z_dir):\n");
			PythonCode += TEXT("    return unreal.Rotator(0,0,0)  # placeholder, handled natively if needed\n");
			
			PythonCode += FString::Printf(TEXT("asset_loaded = unreal.load_asset('%s')\n"), *AssetPath);
			PythonCode += TEXT("is_mesh = isinstance(asset_loaded, unreal.StaticMesh)\n");
			PythonCode += TEXT("is_bp = isinstance(asset_loaded, unreal.Blueprint)\n");
			PythonCode += TEXT("if not is_mesh and not is_bp:\n");
			PythonCode += TEXT("    print(f'ERROR: Asset {asset_loaded} is neither StaticMesh nor Blueprint')\n");
			
			// Actually, doing raycast in Python is via EditorLevelLibrary or SystemLibrary
			// But doing it in C++ is much faster and cleaner. 
			// Let's do the raycasting and actor spawning purely in C++.

			UObject* LoadedAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
			if (!LoadedAsset) { OutError = FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath); return false; }

			UBlueprint* BPAsset = Cast<UBlueprint>(LoadedAsset);
			UStaticMesh* SMAsset = Cast<UStaticMesh>(LoadedAsset);
			if (!BPAsset && !SMAsset) { OutError = TEXT("Asset must be StaticMesh or Blueprint."); return false; }

			int32 Spawns = 0;
			for (int32 i = 0; i < Count; ++i)
			{
				FVector Point(
					CX + FMath::RandRange(-EX, EX),
					CY + FMath::RandRange(-EY, EY),
					200000.0 // Start high
				);

				FHitResult HitResult;
				FVector Start = Point;
				FVector End = Point - FVector(0, 0, 400000.0);
				FCollisionQueryParams Params(SCENE_QUERY_STAT(ScatterQuery), true);

				if (World->LineTraceSingleByChannel(HitResult, Start, End, ECC_Visibility, Params))
				{
					FVector SpawnLoc = HitResult.Location;
					FRotator SpawnRot(0, FMath::RandRange(0.0f, 360.0f), 0);

					if (bAlign)
					{
						FVector RandomYawVec = SpawnRot.Vector();
						FVector ForwardX = FVector::CrossProduct(HitResult.Normal, FVector::RightVector).GetSafeNormal();
						FVector RightY = FVector::CrossProduct(HitResult.Normal, ForwardX).GetSafeNormal();
						
						// Make a rotation representing the local frame oriented with the normal
						FRotator AlignedRot = UKismetMathLibrary::MakeRotFromZX(HitResult.Normal, FVector::ForwardVector);
						// add yaw randomize
						AlignedRot.Yaw += SpawnRot.Yaw;
						SpawnRot = AlignedRot;
					}

					float RandomScale = FMath::RandRange(ScaleMin, ScaleMax);
					FVector Scale3D(RandomScale);

					AActor* SpawnedActor = nullptr;
					if (BPAsset && BPAsset->GeneratedClass)
					{
						SpawnedActor = World->SpawnActor<AActor>(BPAsset->GeneratedClass, SpawnLoc, SpawnRot);
					}
					else if (SMAsset)
					{
						AStaticMeshActor* SMActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), SpawnLoc, SpawnRot);
						if (SMActor && SMActor->GetStaticMeshComponent())
						{
							SMActor->GetStaticMeshComponent()->SetStaticMesh(SMAsset);
							SpawnedActor = SMActor;
						}
					}

					if (SpawnedActor)
					{
						SpawnedActor->SetActorScale3D(Scale3D);
						Spawns++;
					}
				}
			}

			OutStructured->SetNumberField(TEXT("requested_count"), Count);
			OutStructured->SetNumberField(TEXT("spawned_count"), Spawns);
			OutStructured->SetNumberField(TEXT("failed_count"), Count - Spawns);
			if (Spawns == 0)
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("failed"));
				OutError = FString::Printf(TEXT("Scatter failed: 0/%d actors spawned. Check surface collision and asset path."), Count);
				return false;
			}
			OutStructured->SetStringField(TEXT("status"), Spawns == Count ? TEXT("success") : TEXT("partial_success"));
			OutSummary = FString::Printf(TEXT("%s scattered %d/%d instances across %f x %f area."),
				Spawns == Count ? TEXT("Successfully") : TEXT("Partially"),
				Spawns, Count, EX*2, EY*2);
			return true;
		}
	});
}

}
