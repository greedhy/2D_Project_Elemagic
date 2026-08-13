// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP v3.30.x — Worldspace diagnostics tools (WF-T1 subtraction toggles + WF-T3 asserts)
//
// FUNCSPEC_diagnostics.md §1 (減法开关) + §3 (断言库). Any-world generic.
//
//  worldspace_debug_toggle — subtraction switches for root-cause localisation: hide surface
//    bodies (ocean/ice), hide terrain pages, hide atmosphere, unlit viewport. Immediate,
//    reversible (pass the flag = false to restore). Pair with worldspace_headless_shot for
//    bisection ("turn one thing off → shoot → compare").
//
//  worldspace_assert — quantified pass/value/threshold reports (replaces eyeballing):
//    ocean_continuous / no_z_step / seam_contrast / layer_ownership / layout_ok. The metric
//    is computed from the provided sample arrays (the pure-function contract from
//    somol-worldrecipe::assert). Live in-engine terrain sampling to auto-populate samples is
//    annotated pending_ue_live_verify.
//
// UE-LIVE-VERIFY: compile and run this path with the target user's configured Unreal Engine. Written against the
// UE 5.8 public API used elsewhere in this module. The native driver ApplyDebugToggles path
// (FUNCSPEC §1) does not exist on USomolRuntimeTerrainStreamingDriverComponent yet, so the
// toggles are implemented via component-visibility + viewport show-flags (real editor state).

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpErrorHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "LevelEditorViewport.h"

namespace UE::SOMOLMCP
{
namespace DiagDetail
{
	static TSharedPtr<FJsonValue> S(const FString& V) { return MakeShared<FJsonValueString>(V); }

	// Does this actor own any component whose class name contains one of the needlesx
	static bool ActorHasComponentClassLike(AActor* Actor, const TArray<FString>& Needles)
	{
		if (!Actor) return false;
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			if (!Comp) continue;
			const FString ClassName = Comp->GetClass()->GetName();
			for (const FString& Needle : Needles)
			{
				if (ClassName.Contains(Needle)) return true;
			}
		}
		return false;
	}

	// Set visibility on all primitive components of an actor whose class name matches a needle.
	// Returns the number of components toggled.
	static int32 SetComponentVisibilityByClassLike(AActor* Actor, const TArray<FString>& Needles, bool bVisible)
	{
		int32 Count = 0;
		if (!Actor) return 0;
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Comp);
			if (!Prim) continue;
			const FString ClassName = Prim->GetClass()->GetName();
			for (const FString& Needle : Needles)
			{
				if (ClassName.Contains(Needle))
				{
					Prim->SetVisibility(bVisible, true);
					++Count;
					break;
				}
			}
		}
		return Count;
	}

	static double SafeNumber(const TSharedPtr<FJsonValue>& V, double Default = 0.0)
	{
		return (V.IsValid() && (V->Type == EJson::Number)) ? V->AsNumber() : Default;
	}

	static const TArray<TSharedPtr<FJsonValue>>* GetArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Name)
	{
		if (!Obj.IsValid()) return nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj->TryGetArrayField(Name, Arr) && Arr) return Arr;
		return nullptr;
	}
}

using namespace DiagDetail;

// ════════════════════════════════════════════════════════════════════════════
//  worldspace_debug_toggle
// ════════════════════════════════════════════════════════════════════════════
static bool DebugToggle(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& A,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	Out->SetStringField(TEXT("schema"), TEXT("worldspace_debug_toggle.v1"));

	FString WorldError;
	UWorld* World = Context.Services.GetEditorWorld(WorldError);
	if (!World)
	{
		Error = FString::Printf(TEXT("No editor world: %s"), *WorldError);
		SololmcpError::Set(Out, TEXT("NOT_AVAILABLE"), TEXT("world"), Error);
		return false;
	}

	const TArray<FString> TerrainNeedles = {TEXT("TerrainPage"), TEXT("StreamingDriver"), TEXT("TerrainPageMesh")};
	const TArray<FString> SurfaceNeedles = {TEXT("Ocean"), TEXT("Water"), TEXT("GlobePreview"), TEXT("Sea")};
	const TArray<FString> AtmoNeedles = {TEXT("SkyAtmosphere")};

	int32 TerrainActorsToggled = 0;
	int32 SurfaceCompsToggled = 0;
	int32 AtmoCompsToggled = 0;

	// hide_terrain: hide whole terrain-owning actors (page meshes + streaming driver owner).
	if (A->HasField(TEXT("hide_terrain")))
	{
		const bool bHide = A->GetBoolField(TEXT("hide_terrain"));
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (ActorHasComponentClassLike(*It, TerrainNeedles))
			{
				It->SetIsTemporarilyHiddenInEditor(bHide);
				++TerrainActorsToggled;
			}
		}
		Out->SetBoolField(TEXT("hide_terrain_applied"), true);
	}

	// hide_surface_bodies: toggle just the ocean/water surface components (keep terrain visible).
	if (A->HasField(TEXT("hide_surface_bodies")))
	{
		const bool bHide = A->GetBoolField(TEXT("hide_surface_bodies"));
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			SurfaceCompsToggled += SetComponentVisibilityByClassLike(*It, SurfaceNeedles, !bHide);
			// Also cover actors whose class name is a water/ocean actor.
			const FString ActorClass = It->GetClass()->GetName();
			if (ActorClass.Contains(TEXT("Water")) || ActorClass.Contains(TEXT("Ocean")))
			{
				It->SetIsTemporarilyHiddenInEditor(bHide);
			}
		}
		Out->SetBoolField(TEXT("hide_surface_bodies_applied"), true);
	}

	// hide_atmosphere: toggle SkyAtmosphere component visibility.
	if (A->HasField(TEXT("hide_atmosphere")))
	{
		const bool bHide = A->GetBoolField(TEXT("hide_atmosphere"));
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AtmoCompsToggled += SetComponentVisibilityByClassLike(*It, AtmoNeedles, !bHide);
		}
		Out->SetBoolField(TEXT("hide_atmosphere_applied"), true);
	}

	// unlit: flip editor viewport lighting show-flag on all level viewports.
	bool bUnlitApplied = false;
	if (A->HasField(TEXT("unlit")) && GEditor)
	{
		const bool bUnlit = A->GetBoolField(TEXT("unlit"));
		for (FLevelEditorViewportClient* VC : GEditor->GetLevelViewportClients())
		{
			if (!VC) continue;
			VC->EngineShowFlags.SetLighting(!bUnlit);
			VC->Invalidate();
			bUnlitApplied = true;
		}
		Out->SetBoolField(TEXT("unlit_applied"), bUnlitApplied);
	}

	// hide_layers: per-surface-layer-id hide is not yet mapped to a runtime handle.
	if (const TArray<TSharedPtr<FJsonValue>>* Layers = GetArray(A, TEXT("hide_layers")))
	{
		if (Layers->Num() > 0)
		{
			Out->SetStringField(TEXT("hide_layers_note"),
				TEXT("pending_ue_live_verify: per-surface-layer hide requires driver ApplyDebugToggles(HideLayers) which is not yet on USomolRuntimeTerrainStreamingDriverComponent."));
		}
	}

	Out->SetNumberField(TEXT("terrain_actors_toggled"), TerrainActorsToggled);
	Out->SetNumberField(TEXT("surface_components_toggled"), SurfaceCompsToggled);
	Out->SetNumberField(TEXT("atmosphere_components_toggled"), AtmoCompsToggled);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("verification"), TEXT("pending_ue_live_verify: toggles set editor visibility/show-flags; visual effect confirmed via headless_shot after rebuild."));
	Summary = FString::Printf(TEXT("worldspace_debug_toggle: terrain=%d surface=%d atmo=%d unlit=%s"),
		TerrainActorsToggled, SurfaceCompsToggled, AtmoCompsToggled, bUnlitApplied ? TEXT("yes") : TEXT("no"));
	return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  worldspace_assert — pass/value/threshold from provided samples
// ════════════════════════════════════════════════════════════════════════════
static bool WorldspaceAssert(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& A,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	Out->SetStringField(TEXT("schema"), TEXT("worldspace_assert.v1"));

	FString Kind;
	if (!A->TryGetStringField(TEXT("kind"), Kind) || Kind.IsEmpty())
	{
		SololmcpError::MissingParam(Out, TEXT("kind"));
		Error = TEXT("Missing 'kind'.");
		return false;
	}
	TSharedPtr<FJsonObject> Params;
	TryGetObjectField(A, TEXT("params"), Params);
	if (!Params.IsValid())
	{
		Params = MakeShared<FJsonObject>();
	}

	FString Name = Kind;
	bool bPass = false;
	double Value = 0.0;
	double Threshold = 0.0;
	bool bResolved = true;

	if (Kind == TEXT("ocean_continuous"))
	{
		// samples = adjacent surface heights (m); value = max |Δadjacent|; pass = value <= max_step_m.
		Threshold = Params->HasField(TEXT("max_step_m")) ? Params->GetNumberField(TEXT("max_step_m")) : 0.5;
		const TArray<TSharedPtr<FJsonValue>>* Samples = GetArray(Params, TEXT("samples"));
		double MaxStep = 0.0;
		if (Samples && Samples->Num() >= 2)
		{
			for (int32 i = 1; i < Samples->Num(); ++i)
			{
				MaxStep = FMath::Max(MaxStep, FMath::Abs(SafeNumber((*Samples)[i]) - SafeNumber((*Samples)[i - 1])));
			}
		}
		Value = MaxStep;
		bPass = (Samples && Samples->Num() >= 2) ? (Value <= Threshold) : false;
	}
	else if (Kind == TEXT("no_z_step"))
	{
		// edge_pairs = [[a,b],...] adjacent page/layer boundary heights (m); value = max |a-b|.
		Threshold = Params->HasField(TEXT("max_m")) ? Params->GetNumberField(TEXT("max_m")) : 0.25;
		const TArray<TSharedPtr<FJsonValue>>* Pairs = GetArray(Params, TEXT("edge_pairs"));
		double MaxDelta = 0.0;
		int32 Counted = 0;
		if (Pairs)
		{
			for (const TSharedPtr<FJsonValue>& PV : *Pairs)
			{
				if (PV.IsValid() && PV->Type == EJson::Array && PV->AsArray().Num() >= 2)
				{
					const TArray<TSharedPtr<FJsonValue>>& Pr = PV->AsArray();
					MaxDelta = FMath::Max(MaxDelta, FMath::Abs(SafeNumber(Pr[0]) - SafeNumber(Pr[1])));
					++Counted;
				}
			}
		}
		Value = MaxDelta;
		bPass = (Counted > 0) ? (Value <= Threshold) : false;
	}
	else if (Kind == TEXT("seam_contrast"))
	{
		// colors_a / colors_b = paired [r,g,b] across the seam; value = max ΔE (Euclidean).
		Threshold = Params->HasField(TEXT("max_de")) ? Params->GetNumberField(TEXT("max_de")) : 12.0;
		const TArray<TSharedPtr<FJsonValue>>* CA = GetArray(Params, TEXT("colors_a"));
		const TArray<TSharedPtr<FJsonValue>>* CB = GetArray(Params, TEXT("colors_b"));
		double MaxDE = 0.0;
		int32 Counted = 0;
		if (CA && CB)
		{
			const int32 N = FMath::Min(CA->Num(), CB->Num());
			for (int32 i = 0; i < N; ++i)
			{
				const TSharedPtr<FJsonValue>& VA = (*CA)[i];
				const TSharedPtr<FJsonValue>& VB = (*CB)[i];
				if (VA->Type == EJson::Array && VB->Type == EJson::Array)
				{
					const TArray<TSharedPtr<FJsonValue>>& Aa = VA->AsArray();
					const TArray<TSharedPtr<FJsonValue>>& Bb = VB->AsArray();
					if (Aa.Num() >= 3 && Bb.Num() >= 3)
					{
						const double DR = SafeNumber(Aa[0]) - SafeNumber(Bb[0]);
						const double DG = SafeNumber(Aa[1]) - SafeNumber(Bb[1]);
						const double DB = SafeNumber(Aa[2]) - SafeNumber(Bb[2]);
						MaxDE = FMath::Max(MaxDE, FMath::Sqrt(DR * DR + DG * DG + DB * DB));
						++Counted;
					}
				}
			}
		}
		Value = MaxDE;
		bPass = (Counted > 0) ? (Value <= Threshold) : false;
		Out->SetStringField(TEXT("metric_note"), TEXT("ΔE is Euclidean over the supplied color space (not full CIELab)."));
	}
	else if (Kind == TEXT("layer_ownership"))
	{
		// samples = [{value,min,max}]; value = count of out-of-range samples; pass = 0.
		Threshold = 0.0;
		const TArray<TSharedPtr<FJsonValue>>* Samples = GetArray(Params, TEXT("samples"));
		int32 OutOfRange = 0;
		int32 Counted = 0;
		if (Samples)
		{
			for (const TSharedPtr<FJsonValue>& SV : *Samples)
			{
				if (SV.IsValid() && SV->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> O = SV->AsObject();
					const double Val = O->HasField(TEXT("value")) ? O->GetNumberField(TEXT("value")) : 0.0;
					const double Min = O->HasField(TEXT("min")) ? O->GetNumberField(TEXT("min")) : -TNumericLimits<double>::Max();
					const double Max = O->HasField(TEXT("max")) ? O->GetNumberField(TEXT("max")) : TNumericLimits<double>::Max();
					if (Val < Min || Val > Max) ++OutOfRange;
					++Counted;
				}
			}
		}
		Value = OutOfRange;
		bPass = (Counted > 0) ? (OutOfRange == 0) : false;
	}
	else if (Kind == TEXT("layout_ok"))
	{
		// ok = layout_validate report boolean (already computed upstream).
		Threshold = 1.0;
		const bool bOk = Params->HasField(TEXT("ok")) && Params->GetBoolField(TEXT("ok"));
		Value = bOk ? 1.0 : 0.0;
		bPass = bOk;
	}
	else
	{
		bResolved = false;
		SololmcpError::Set(Out, TEXT("INVALID_TYPE"), TEXT("kind"),
			TEXT("kind must be one of: ocean_continuous, no_z_step, seam_contrast, layer_ownership, layout_ok."));
		Error = FString::Printf(TEXT("Unknown assert kind '%s'."), *Kind);
		return false;
	}

	// AssertReport contract: {name, pass, value, threshold}.
	Out->SetStringField(TEXT("name"), Name);
	Out->SetBoolField(TEXT("pass"), bPass);
	Out->SetNumberField(TEXT("value"), Value);
	Out->SetNumberField(TEXT("threshold"), Threshold);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("verification"), TEXT("pending_ue_live_verify: metric computed from supplied samples; live in-engine sampling auto-population is future work."));
	Summary = FString::Printf(TEXT("worldspace_assert '%s': %s (value=%.4f, threshold=%.4f)"),
		*Name, bPass ? TEXT("PASS") : TEXT("FAIL"), Value, Threshold);
	return bResolved;
}

// ════════════════════════════════════════════════════════════════════════════
//  Registration
// ════════════════════════════════════════════════════════════════════════════
void RegisterWorldspaceDiagnosticsTools(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("worldspace_debug_toggle"),
		TEXT("Subtraction switches for root-cause localisation: hide_surface_bodies / hide_terrain / hide_atmosphere / hide_layers / unlit. Immediate + reversible; pair with worldspace_headless_shot for bisection."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("hide_surface_bodies"), FSololmcpSchemaBuilder::Boolean(TEXT("Hide ocean/ice surface bodies (keep terrain)."))},
			{TEXT("hide_terrain"), FSololmcpSchemaBuilder::Boolean(TEXT("Hide terrain page actors."))},
			{TEXT("hide_atmosphere"), FSololmcpSchemaBuilder::Boolean(TEXT("Hide SkyAtmosphere."))},
			{TEXT("hide_layers"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Surface layer ids to hide."))},
			{TEXT("unlit"), FSololmcpSchemaBuilder::Boolean(TEXT("Force unlit viewport (vertex color, no lighting artefacts)."))}
		}),
		DebugToggle
	});
	Registry.Register({
		TEXT("worldspace_assert"),
		TEXT("Quantified pass/value/threshold assertion (replaces eyeballing): ocean_continuous | no_z_step | seam_contrast | layer_ownership | layout_ok. Metric computed from provided sample arrays."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("kind"), FSololmcpSchemaBuilder::String(TEXT("Assertion kind."),
				{TEXT("ocean_continuous"), TEXT("no_z_step"), TEXT("seam_contrast"), TEXT("layer_ownership"), TEXT("layout_ok")})},
			{TEXT("params"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Kind-specific samples + threshold."))}
		}, {TEXT("kind")}),
		WorldspaceAssert
	});
}
}
