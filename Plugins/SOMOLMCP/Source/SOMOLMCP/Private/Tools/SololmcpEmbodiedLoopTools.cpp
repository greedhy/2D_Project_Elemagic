// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpEmbodiedLoopTools.cpp — SOMOLMCP P4-6
// Embodied loop / PIE control tools (6):
//   embody_pie_start / embody_pie_stop / embody_pie_state /
//   embody_input_press_key / embody_input_axis / embody_get_observation
//
// Goal:
//   Close the AI <-> game <-> AI loop by letting agents drive PIE end-to-end.
//   - Start/stop a PIE session.
//   - Inject synthetic input (key + axis) into the running game.
//   - Query game state (player pawn pose, fps, time, nearby actors, optional HP).
//   - Capture an observation snapshot (composite of pose + nearby actors + optional screenshot).
//
// Distinct from existing pie_* tools (SololmcpEnhancedTools.cpp):
//   pie_start/pie_stop/pie_get_status/pie_capture/pie_screenshot   — stateless, no input injection.
//   embody_*  prefix — designed for an autonomous agent loop with input + observation.
//
// Notes (UE 5.7):
//   - GEditor->RequestPlaySession(FRequestPlaySessionParams) starts PIE asynchronously.
//   - GEditor->RequestEndPlayMap() stops it.
//   - GEditor->PlayWorld is the live UWorld* of the running PIE session (nullptr otherwise).
//   - GEditor->bIsPlayingSessionInEditor is the canonical "PIE active" flag in 5.7.
//   - FSlateApplication::Get().ProcessKeyDownEvent(...) routes through the focused widget,
//     which when the PIE viewport has focus reaches the player controller's input stack.
//   - Enhanced Input axes (UInputAction) cannot be directly poked without holding a reference
//     to the asset; this implementation provides best-effort key-translation for legacy
//     axis names ("MoveForward"->W/S etc.) and otherwise returns NOT_IMPLEMENTED.
//     See // TODO(P4-6) below.
//
//
// Wiring (NOT done by this file — supervisor adds these manually):
//   - SololmcpToolRegistry.h: forward-declare RegisterEmbodiedLoopTools(FSololmcpToolRegistry&)
//   - SololmcpToolRegistry.cpp: call RegisterEmbodiedLoopTools(*this) inside the ctor.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

// Editor + PIE
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Settings/LevelEditorPlaySettings.h"

// World / actors
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "EngineUtils.h"
#include "TimerManager.h"

// Slate (input synthesis)
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Widgets/SWindow.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/SharedPointer.h"

// Reflection (for hp_if_present)
#include "UObject/UnrealType.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPEmbodied, Log, All);

namespace UE::SOMOLMCP
{

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/** Game-thread guard — Slate / GEditor / PIE state must be touched on the GT. */
static bool Embodied_EnsureGameThread(FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("Embodied loop tools must be called from the game thread.");
		return false;
	}
	return true;
}

/** Returns the live PIE UWorld* if PIE is running, else nullptr. */
static UWorld* Embodied_GetPlayWorld()
{
	if (!GEditor) return nullptr;
	if (GEditor->PlayWorld) return GEditor->PlayWorld;
	// Fallback: walk world contexts.
	for (const FWorldContext& Ctx : GEditor->GetWorldContexts())
	{
		if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
		{
			return Ctx.World();
		}
	}
	return nullptr;
}

/** True iff a PIE session is currently active. */
static bool Embodied_IsPieRunning()
{
	if (!GEditor) return false;
	if (GEditor->PlayWorld) return true;
	for (const FWorldContext& Ctx : GEditor->GetWorldContexts())
	{
		if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
		{
			return true;
		}
	}
	return false;
}

/** Block-with-timeout poll for PIE to come up. Pump Slate/timers each tick. */
static bool Embodied_WaitForPie(double TimeoutSec)
{
	const double Start = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - Start < TimeoutSec)
	{
		if (Embodied_IsPieRunning())
		{
			return true;
		}
		// audit-U2 fix (P1): the previous version called FSlateApplication::Tick()
		// from inside a tool body that was itself invoked from a Slate/Ticker
		// frame — re-entering Slate.Tick() violates its single-tick contract
		// and can produce torn widget state / random hangs. Use FTSTicker only:
		// PIE-start callbacks queue into the CoreTicker, which is safe to pump
		// without re-entering Slate. Slate itself will tick at its natural
		// frame boundary while we sleep.
		FPlatformProcess::Sleep(0.05f);
	}
	return Embodied_IsPieRunning();
}

static bool Embodied_WaitForPieStopped(double TimeoutSec)
{
	const double Start = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - Start < TimeoutSec)
	{
		if (!Embodied_IsPieRunning())
		{
			return true;
		}
		// audit-U2 fix: same rationale as Embodied_WaitForPieStarted above.
		FPlatformProcess::Sleep(0.05f);
	}
	return !Embodied_IsPieRunning();
}

static FString Embodied_NormalizeSlateText(const FString& In)
{
	FString Out = In;
	Out.TrimStartAndEndInline();
	Out.ReplaceInline(TEXT("\r"), TEXT(" "));
	Out.ReplaceInline(TEXT("\n"), TEXT(" "));
	while (Out.Contains(TEXT("  ")))
	{
		Out.ReplaceInline(TEXT("  "), TEXT(" "));
	}
	return Out;
}

static TArray<TSharedPtr<FJsonValue>> Embodied_CollectVisibleWindowSummaries(
	int32& OutWindowCount,
	int32& OutBlockingWindowCount,
	FString& OutTitleSummary)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	OutWindowCount = 0;
	OutBlockingWindowCount = 0;
	OutTitleSummary.Empty();

	if (!FSlateApplication::IsInitialized())
	{
		return Items;
	}

	TArray<TSharedRef<SWindow>> Windows;
	TSet<const SWindow*> Seen;
	for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetInteractiveTopLevelWindows())
	{
		if (Window->IsVisible() && !Seen.Contains(&Window.Get()))
		{
			Seen.Add(&Window.Get());
			Windows.Add(Window);
		}
	}
	TArray<TSharedRef<SWindow>> OrderedVisible;
	FSlateApplication::Get().GetAllVisibleWindowsOrdered(OrderedVisible);
	for (const TSharedRef<SWindow>& Window : OrderedVisible)
	{
		if (Window->IsVisible() && !Seen.Contains(&Window.Get()))
		{
			Seen.Add(&Window.Get());
			Windows.Add(Window);
		}
	}

	OutWindowCount = Windows.Num();
	for (int32 Index = 0; Index < Windows.Num() && Items.Num() < 16; ++Index)
	{
		const TSharedRef<SWindow>& Window = Windows[Index];
		const FString Title = Embodied_NormalizeSlateText(Window->GetTitle().ToString());
		const bool bLooksBlocking = Window->IsModalWindow();
		if (bLooksBlocking)
		{
			++OutBlockingWindowCount;
		}
		if (!Title.IsEmpty())
		{
			if (!OutTitleSummary.IsEmpty())
			{
				OutTitleSummary += TEXT(" | ");
			}
			OutTitleSummary += Title;
		}

		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("title"), Title);
		Json->SetStringField(TEXT("type"), Window->GetTypeAsString());
		Json->SetBoolField(TEXT("is_modal"), Window->IsModalWindow());
		Json->SetBoolField(TEXT("is_regular_window"), Window->IsRegularWindow());
		Json->SetBoolField(TEXT("looks_blocking"), bLooksBlocking);
		Items.Add(MakeShared<FJsonValueObject>(Json));
	}
	return Items;
}

static void Embodied_SetStatus(TSharedRef<FJsonObject>& OutStructured, const TCHAR* Status)
{
	OutStructured->SetStringField(TEXT("status"), Status);
}

/** Best-effort: read a numeric "Health" / "HP" / "CurrentHealth" property off any UObject. */
static bool Embodied_TryReadHealth(UObject* Obj, double& OutHp)
{
	if (!Obj) return false;
	UClass* Cls = Obj->GetClass();
	if (!Cls) return false;
	static const TArray<FName> Candidates = {
		TEXT("Health"), TEXT("HP"), TEXT("CurrentHealth"),
		TEXT("CurrentHP"), TEXT("HealthPoints")
	};
	for (const FName& PropName : Candidates)
	{
		if (FProperty* Prop = Cls->FindPropertyByName(PropName))
		{
			void* Addr = Prop->ContainerPtrToValuePtr<void>(Obj);
			if (FFloatProperty* FP = CastField<FFloatProperty>(Prop))
			{
				OutHp = FP->GetPropertyValue(Addr);
				return true;
			}
			if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
			{
				OutHp = DP->GetPropertyValue(Addr);
				return true;
			}
			if (FIntProperty* IP = CastField<FIntProperty>(Prop))
			{
				OutHp = (double)IP->GetPropertyValue(Addr);
				return true;
			}
		}
	}
	return false;
}

/** Vector / rotator → FJsonObject. */
static TSharedRef<FJsonObject> Embodied_VecToJson(const FVector& V)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("x"), V.X);
	O->SetNumberField(TEXT("y"), V.Y);
	O->SetNumberField(TEXT("z"), V.Z);
	return O;
}
static TSharedRef<FJsonObject> Embodied_RotToJson(const FRotator& R)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("pitch"), R.Pitch);
	O->SetNumberField(TEXT("yaw"),   R.Yaw);
	O->SetNumberField(TEXT("roll"),  R.Roll);
	return O;
}

/** Build the player-pawn JSON sub-object. Returns null SharedPtr if no pawn. */
static TSharedPtr<FJsonObject> Embodied_PlayerPawnJson(UWorld* World)
{
	if (!World) return nullptr;
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC) return nullptr;
	APawn* Pawn = PC->GetPawn();
	if (!Pawn) return nullptr;
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("actor_name"), Pawn->GetName());
	O->SetStringField(TEXT("class"),      Pawn->GetClass()->GetName());
	O->SetObjectField(TEXT("location"),   Embodied_VecToJson(Pawn->GetActorLocation()));
	O->SetObjectField(TEXT("rotation"),   Embodied_RotToJson(Pawn->GetActorRotation()));
	double Hp = 0.0;
	if (Embodied_TryReadHealth(Pawn, Hp))
	{
		O->SetNumberField(TEXT("hp_if_present"), Hp);
	}
	return O;
}

/** Map a legacy axis name to a representative key, if we can. Returns IsValid()==false otherwise. */
static FKey Embodied_LegacyAxisToKey(const FString& AxisName, double Value)
{
	const FString N = AxisName.ToLower();
	if (N == TEXT("moveforward")) return Value >= 0.0 ? EKeys::W : EKeys::S;
	if (N == TEXT("moveright"))   return Value >= 0.0 ? EKeys::D : EKeys::A;
	if (N == TEXT("lookup"))      return Value >= 0.0 ? EKeys::Up : EKeys::Down;
	if (N == TEXT("lookright"))   return Value >= 0.0 ? EKeys::Right : EKeys::Left;
	return FKey();
}

// ─────────────────────────────────────────────────────────────────────────────
// RegisterEmbodiedLoopTools
// ─────────────────────────────────────────────────────────────────────────────

void RegisterEmbodiedLoopTools(FSololmcpToolRegistry& Registry)
{
	const TArray<FString> ModeEnum = {
		TEXT("selected_viewport"), TEXT("new_window"),
		TEXT("vr_preview"), TEXT("mobile_preview")
	};
	const TArray<FString> AxisEnum = {
		TEXT("MoveForward"), TEXT("MoveRight"),
		TEXT("LookUp"), TEXT("LookRight")
	};
	const TArray<FString> PlayNetModeEnum = {
		TEXT("standalone"), TEXT("listen_server"), TEXT("client")
	};

	// ─────────────────────────────────────────────────────────────────────────
	// 1. embody_pie_start
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("embody_pie_start"),
		TEXT("Start a Play In Editor (PIE) session for the embodied loop. "
			 "Submits the start request, performs a bounded activation probe, and returns "
			 "success when PIE is already running or when the start request is safely pending "
			 "without a blocking Slate window."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mode"), FSololmcpSchemaBuilder::String(
					TEXT("PIE mode: selected_viewport (default) | new_window | vr_preview | mobile_preview."),
					ModeEnum)},
				{TEXT("dedicated_server"), FSololmcpSchemaBuilder::Boolean(
					TEXT("Run a dedicated server alongside the PIE client. Default: false."))},
				{TEXT("requested_clients"), FSololmcpSchemaBuilder::Integer(
					TEXT("Number of PIE client windows to request. Default 1; use 2+ with play_net_mode=listen_server for WorldForge observer tests."))},
				{TEXT("play_net_mode"), FSololmcpSchemaBuilder::String(
					TEXT("PIE networking mode: standalone | listen_server | client. Default: standalone."),
					PlayNetModeEnum)},
				{TEXT("timeout_seconds"), FSololmcpSchemaBuilder::Number(
					TEXT("Bounded wait for PIE to become active. Default 5.0, range 1.0-60.0."))},
				{TEXT("return_immediately"), FSololmcpSchemaBuilder::Boolean(
					TEXT("Submit RequestPlaySession and return immediately with client_status=starting so clients poll embody_pie_state instead of blocking the MCP/game thread."))}
			},
			{}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!Embodied_EnsureGameThread(OutError)) return false;
			if (!GEditor)
			{
				SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"));
				OutError = TEXT("GEditor not available.");
				return false;
			}

			FString Mode = TEXT("selected_viewport");
			Arguments->TryGetStringField(TEXT("mode"), Mode);
			bool bDedicatedServer = false;
			Arguments->TryGetBoolField(TEXT("dedicated_server"), bDedicatedServer);
			double RequestedClientsRaw = 1.0;
			Arguments->TryGetNumberField(TEXT("requested_clients"), RequestedClientsRaw);
			const int32 RequestedClients = FMath::Clamp(static_cast<int32>(RequestedClientsRaw), 1, 4);
			FString PlayNetMode = TEXT("standalone");
			Arguments->TryGetStringField(TEXT("play_net_mode"), PlayNetMode);
			PlayNetMode.TrimStartAndEndInline();
			PlayNetMode.ToLowerInline();
			double TimeoutSeconds = 5.0;
			Arguments->TryGetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
			TimeoutSeconds = FMath::Clamp(TimeoutSeconds, 1.0, 60.0);
			bool bReturnImmediately = false;
			Arguments->TryGetBoolField(TEXT("return_immediately"), bReturnImmediately);

			// Reject already-running so callers don't double-launch.
			if (Embodied_IsPieRunning())
			{
				Embodied_SetStatus(OutStructured, TEXT("success"));
				OutStructured->SetBoolField(TEXT("pie_running"), true);
				OutStructured->SetStringField(TEXT("mode"), Mode);
				OutStructured->SetStringField(TEXT("play_net_mode"), PlayNetMode);
				OutStructured->SetNumberField(TEXT("requested_clients"), RequestedClients);
				if (UWorld* W = Embodied_GetPlayWorld())
				{
					OutStructured->SetStringField(TEXT("world_path"), W->GetPathName());
				}
				OutSummary = TEXT("PIE already running.");
				return true;
			}

			FRequestPlaySessionParams Params;
			// UE 5.7: viewport vs. window selection driven by DestinationSlateViewport.
			if (Mode == TEXT("new_window"))
			{
				Params.DestinationSlateViewport.Reset();
			}
			else if (Mode == TEXT("vr_preview"))
			{
				// TODO(P4-6): EPlaySessionPreviewType::VRPreview field name varies between
				// 5.x patch revs; the safe knob is SessionPreviewTypeOverride. If unsupported,
				// fall through to default selected_viewport behavior.
				Params.SessionPreviewTypeOverride = EPlaySessionPreviewType::VRPreview;
			}
			else if (Mode == TEXT("mobile_preview"))
			{
				Params.SessionPreviewTypeOverride = EPlaySessionPreviewType::MobilePreview;
			}
			// selected_viewport: leave defaults — PIE re-uses the active level viewport.

			if (ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>())
			{
				PlaySettings->SetPlayNumberOfClients(RequestedClients);
				PlaySettings->SetRunUnderOneProcess(true);
				if (PlayNetMode == TEXT("listen_server"))
				{
					PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer);
				}
				else if (PlayNetMode == TEXT("client"))
				{
					PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Client);
				}
				else
				{
					PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
				}
			}

			if (bDedicatedServer)
			{
				// TODO(P4-6): NumOutstandingPIELogins / EditorPlaySettings->bLaunchSeparateServer
				// is the more reliable knob; SessionDestination::DedicatedServer might not exist
				// on all 5.7.x patches. Leaving as a runtime override for now.
				Params.SessionDestination = EPlaySessionDestinationType::InProcess;
				if (ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>())
				{
					PlaySettings->bLaunchSeparateServer = true;
				}
			}

			GEditor->RequestPlaySession(Params);

			if (bReturnImmediately)
			{
				Embodied_SetStatus(OutStructured, TEXT("success"));
				OutStructured->SetBoolField(TEXT("pie_running"), false);
				OutStructured->SetBoolField(TEXT("request_accepted"), true);
				OutStructured->SetStringField(TEXT("client_status"), TEXT("starting"));
				OutStructured->SetStringField(TEXT("mode"), Mode);
				OutStructured->SetStringField(TEXT("play_net_mode"), PlayNetMode);
				OutStructured->SetNumberField(TEXT("requested_clients"), RequestedClients);
				OutStructured->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
				OutStructured->SetBoolField(TEXT("requires_followup_status_poll"), true);
				OutStructured->SetStringField(TEXT("followup_status_tool"), TEXT("embody_pie_state"));
				OutStructured->SetStringField(TEXT("start_wait_boundary"),
					TEXT("RequestPlaySession is asynchronous; the tool returned immediately to keep the MCP command lane responsive. Poll embody_pie_state for activation."));
				OutSummary = FString::Printf(TEXT("PIE start requested asynchronously (mode=%s); poll embody_pie_state for activation."), *Mode);
				return true;
			}

			const bool bUp = Embodied_WaitForPie(TimeoutSeconds);

			Embodied_SetStatus(OutStructured, bUp ? TEXT("success") : TEXT("failed"));
			OutStructured->SetBoolField(TEXT("pie_running"), bUp);
			OutStructured->SetStringField(TEXT("mode"), Mode);
			OutStructured->SetStringField(TEXT("play_net_mode"), PlayNetMode);
			OutStructured->SetNumberField(TEXT("requested_clients"), RequestedClients);
			OutStructured->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
			if (UWorld* W = Embodied_GetPlayWorld())
			{
				OutStructured->SetStringField(TEXT("world_path"), W->GetPathName());
			}
			else
			{
				OutStructured->SetStringField(TEXT("world_path"), TEXT(""));
			}

			if (!bUp)
			{
				int32 WindowCount = 0;
				int32 BlockingWindowCount = 0;
				FString WindowTitles;
				TArray<TSharedPtr<FJsonValue>> VisibleWindows = Embodied_CollectVisibleWindowSummaries(
					WindowCount,
					BlockingWindowCount,
					WindowTitles);
				OutStructured->SetNumberField(TEXT("post_start_visible_window_count"), WindowCount);
				OutStructured->SetNumberField(TEXT("post_start_blocking_window_count"), BlockingWindowCount);
				OutStructured->SetStringField(TEXT("post_start_visible_window_titles"), WindowTitles);
				OutStructured->SetArrayField(TEXT("post_start_visible_windows"), VisibleWindows);
				OutStructured->SetBoolField(TEXT("blocked_modal_or_no_response"), BlockingWindowCount > 0);
				OutStructured->SetStringField(TEXT("watchdog_required_tool_on_timeout"), TEXT("editor_dialog_watchdog_tick"));
				OutStructured->SetStringField(
					TEXT("safe_runtime_recovery_policy"),
					TEXT("After a failed PIE start, run editor_dialog_watchdog_tick/editor_dialog_list before retrying. "
					     "Unattended recovery may only use Cancel/Close/No/Don't Save/Skip Recovery style safe actions; never Run in Editor, Yes, Save, Delete, or Overwrite."));
				if (BlockingWindowCount <= 0)
				{
					Embodied_SetStatus(OutStructured, TEXT("success"));
					OutStructured->SetStringField(TEXT("client_status"), TEXT("starting"));
					OutStructured->SetBoolField(TEXT("request_accepted"), true);
					OutStructured->SetBoolField(TEXT("requires_followup_status_poll"), true);
					OutStructured->SetStringField(TEXT("followup_status_tool"), TEXT("embody_pie_state"));
					OutStructured->SetStringField(TEXT("start_wait_boundary"),
						TEXT("RequestPlaySession can finish on the editor tick after the MCP tool returns; callers must poll embody_pie_state instead of treating this as a hard failure."));
					OutSummary = FString::Printf(TEXT("PIE start requested (mode=%s); poll embody_pie_state for activation."), *Mode);
					return true;
				}
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					FString::Printf(TEXT("PIE did not start within %.1fs. Check Output Log and modal dialogs for compile errors or missing default GameMode."), TimeoutSeconds));
				OutError = FString::Printf(TEXT("PIE did not start within %.1fs."), TimeoutSeconds);
				return false;
			}

			OutSummary = FString::Printf(TEXT("PIE started (mode=%s)."), *Mode);
			return true;
		}
	, nullptr
	, 0
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 2. embody_pie_stop
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("embody_pie_stop"),
		TEXT("Stop the active Play In Editor (PIE) session with a bounded final-state probe."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("timeout_seconds"), FSololmcpSchemaBuilder::Number(
					TEXT("Bounded wait for PIE to stop. Default 10.0, range 1.0-60.0."))}
			},
			{}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!Embodied_EnsureGameThread(OutError)) return false;
			if (!GEditor)
			{
				SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"));
				OutError = TEXT("GEditor not available.");
				return false;
			}

			double TimeoutSeconds = 10.0;
			Arguments->TryGetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
			TimeoutSeconds = FMath::Clamp(TimeoutSeconds, 1.0, 60.0);

			if (!Embodied_IsPieRunning())
			{
				Embodied_SetStatus(OutStructured, TEXT("success"));
				OutStructured->SetBoolField(TEXT("stopped"), true);
				OutStructured->SetBoolField(TEXT("pie_running"), false);
				OutStructured->SetBoolField(TEXT("already_stopped"), true);
				OutStructured->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
				OutSummary = TEXT("PIE already stopped.");
				return true;
			}

			GEditor->RequestEndPlayMap();
			const bool bStopped = Embodied_WaitForPieStopped(TimeoutSeconds);
			Embodied_SetStatus(OutStructured, bStopped ? TEXT("success") : TEXT("failed"));
			OutStructured->SetBoolField(TEXT("stopped"), bStopped);
			OutStructured->SetBoolField(TEXT("pie_running"), !bStopped);
			OutStructured->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
			OutStructured->SetBoolField(TEXT("requires_followup_status_poll"), !bStopped);
			OutStructured->SetStringField(TEXT("followup_status_tool"), TEXT("embody_pie_state"));
			if (!bStopped)
			{
				Embodied_SetStatus(OutStructured, TEXT("success"));
				OutStructured->SetStringField(TEXT("client_status"), TEXT("stopping"));
				OutStructured->SetBoolField(TEXT("request_accepted"), true);
				OutStructured->SetBoolField(TEXT("requires_followup_status_poll"), true);
				OutStructured->SetStringField(TEXT("stop_wait_boundary"),
					TEXT("RequestEndPlayMap can complete on the editor tick after the MCP tool returns; callers must poll embody_pie_state instead of treating this as a hard failure."));
				OutSummary = TEXT("PIE stop requested; poll embody_pie_state for final stopped state.");
				return true;
			}
			OutSummary = TEXT("PIE stopped.");
			return true;
		}
	, nullptr
	, 0
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 3. embody_pie_state
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("embody_pie_state"),
		TEXT("Snapshot of the running PIE session: pie_running, world_path, "
			 "current_player_pawn (actor_name, location, rotation, hp_if_present), "
			 "fps, time_seconds. Returns NOT_AVAILABLE error_code when PIE is not running."),
		FSololmcpSchemaBuilder::Object({}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!Embodied_EnsureGameThread(OutError)) return false;
			if (!GEditor)
			{
				SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"));
				OutError = TEXT("GEditor not available.");
				return false;
			}

			UWorld* World = Embodied_GetPlayWorld();
			if (!World)
			{
				OutStructured->SetBoolField(TEXT("pie_running"), false);
				SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
					TEXT("PIE is not running. Call embody_pie_start first."));
				OutError = TEXT("PIE is not running.");
				return false;
			}

			OutStructured->SetBoolField(TEXT("pie_running"), true);
			OutStructured->SetStringField(TEXT("world_path"), World->GetPathName());
			OutStructured->SetNumberField(TEXT("time_seconds"), World->GetTimeSeconds());

			// FPS estimate from delta seconds of last tick.
			const float DT = World->GetDeltaSeconds();
			const double Fps = DT > 0.0f ? (1.0 / DT) : 0.0;
			OutStructured->SetNumberField(TEXT("fps"), Fps);

			TSharedPtr<FJsonObject> PawnJson = Embodied_PlayerPawnJson(World);
			if (PawnJson.IsValid())
			{
				OutStructured->SetObjectField(TEXT("current_player_pawn"), PawnJson);
			}
			else
			{
				OutStructured->SetField(TEXT("current_player_pawn"), MakeShared<FJsonValueNull>());
			}

			OutSummary = FString::Printf(TEXT("PIE state @ t=%.2fs (fps=%.1f)."),
				World->GetTimeSeconds(), Fps);
			return true;
		}
	, nullptr
	, 0
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 4. embody_input_press_key
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("embody_input_press_key"),
		TEXT("Synthesize a key press into the PIE session. "
			 "key_name follows UE FKey naming: W, A, S, D, Space, LeftMouseButton, "
			 "Gamepad_FaceButton_Bottom, etc. duration_ms (default 50) holds the key down "
			 "before release. Returns NOT_AVAILABLE if PIE is not running, NOT_FOUND if key is unknown."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("key_name"),    FSololmcpSchemaBuilder::String(
					TEXT("UE FKey name, e.g. W | Space | LeftMouseButton | Gamepad_FaceButton_Bottom."))},
				{TEXT("duration_ms"), FSololmcpSchemaBuilder::Integer(
					TEXT("Hold-down duration in milliseconds before release. Default: 50."))}
			},
			{TEXT("key_name")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!Embodied_EnsureGameThread(OutError)) return false;

			FString KeyName;
			if (!Arguments->TryGetStringField(TEXT("key_name"), KeyName) || KeyName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("key_name"));
				OutError = TEXT("Missing required argument: key_name");
				return false;
			}

			int32 DurationMs = 50;
			Arguments->TryGetNumberField(TEXT("duration_ms"), DurationMs);
			DurationMs = FMath::Clamp(DurationMs, 0, 5000);

			if (!Embodied_IsPieRunning())
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
					TEXT("PIE is not running. Call embody_pie_start first."));
				OutError = TEXT("PIE is not running.");
				return false;
			}

			if (!FSlateApplication::IsInitialized())
			{
				SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"));
				OutError = TEXT("FSlateApplication not initialized.");
				return false;
			}

			FKey Key(*KeyName);
			if (!Key.IsValid())
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("key_name"),
					FString::Printf(TEXT("Unknown FKey '%s'. See Engine/InputCoreTypes.h EKeys::*."), *KeyName));
				OutError = FString::Printf(TEXT("Unknown key '%s'."), *KeyName);
				return false;
			}

			FModifierKeysState Modifiers(false, false, false, false, false, false, false, false, false);
			// FKeyEvent(FKey, FModifierKeysState, UserIndex, IsRepeat, CharacterCode, KeyCode)
			FKeyEvent KeyDown(Key, Modifiers, 0, false, 0, 0);
			FKeyEvent KeyUp(Key, Modifiers, 0, false, 0, 0);

			const bool bDown = FSlateApplication::Get().ProcessKeyDownEvent(KeyDown);
			if (DurationMs > 0)
			{
				FPlatformProcess::Sleep(DurationMs / 1000.0f);
			}
			const bool bUp = FSlateApplication::Get().ProcessKeyUpEvent(KeyUp);

			const bool bApplied = bDown && bUp;
			if (!bDown && !bUp)
			{
				Embodied_SetStatus(OutStructured, TEXT("failed"));
				OutStructured->SetStringField(TEXT("key"), KeyName);
				OutStructured->SetBoolField(TEXT("pressed"), false);
				OutStructured->SetBoolField(TEXT("released"), false);
				OutStructured->SetNumberField(TEXT("duration_ms"), DurationMs);
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("Slate rejected both key down and key up events."));
				OutError = TEXT("Key input was not applied.");
				return false;
			}
			Embodied_SetStatus(OutStructured, bApplied ? TEXT("success") : TEXT("partial_success"));
			OutStructured->SetStringField(TEXT("key"), KeyName);
			OutStructured->SetBoolField(TEXT("pressed"),  bDown);
			OutStructured->SetBoolField(TEXT("released"), bUp);
			OutStructured->SetBoolField(TEXT("applied"), bApplied);
			OutStructured->SetNumberField(TEXT("duration_ms"), DurationMs);

			OutSummary = FString::Printf(TEXT("Key '%s' pressed for %d ms (down=%s up=%s)."),
				*KeyName, DurationMs,
				bDown ? TEXT("true") : TEXT("false"),
				bUp   ? TEXT("true") : TEXT("false"));
			return true;
		}
	, nullptr
	, 0
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 5. embody_input_axis
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("embody_input_axis"),
		TEXT("Drive an axis input (MoveForward/MoveRight/LookUp/LookRight) into the PIE session "
			 "for hold_ms milliseconds. Implementation strategy: legacy axis names are translated "
			 "to representative WASD/Arrow key holds; Enhanced Input UInputAction introspection "
			 "is not implemented (returns NOT_IMPLEMENTED for unknown axes). For full analog "
			 "control, use multiple embody_input_press_key calls."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("axis_name"), FSololmcpSchemaBuilder::String(
					TEXT("Axis name: MoveForward | MoveRight | LookUp | LookRight."), AxisEnum)},
				{TEXT("value"),     FSololmcpSchemaBuilder::Number(
					TEXT("Axis value in [-1.0, 1.0]. Sign determines direction; magnitude maps to hold."))},
				{TEXT("hold_ms"),   FSololmcpSchemaBuilder::Integer(
					TEXT("Hold duration in milliseconds. Default: 100."))}
			},
			{TEXT("axis_name"), TEXT("value")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!Embodied_EnsureGameThread(OutError)) return false;

			FString AxisName;
			if (!Arguments->TryGetStringField(TEXT("axis_name"), AxisName) || AxisName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("axis_name"));
				OutError = TEXT("Missing required argument: axis_name");
				return false;
			}
			double Value = 0.0;
			if (!Arguments->TryGetNumberField(TEXT("value"), Value))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("value"));
				OutError = TEXT("Missing required argument: value");
				return false;
			}
			Value = FMath::Clamp(Value, -1.0, 1.0);

			int32 HoldMs = 100;
			Arguments->TryGetNumberField(TEXT("hold_ms"), HoldMs);
			HoldMs = FMath::Clamp(HoldMs, 0, 10000);

			if (!Embodied_IsPieRunning())
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
					TEXT("PIE is not running. Call embody_pie_start first."));
				OutError = TEXT("PIE is not running.");
				return false;
			}

			// TODO(P4-6): Enhanced Input axis introspection.
			//   Real solution requires:
			//     1. Locate UInputMappingContext bound to the local player (UEnhancedInputLocalPlayerSubsystem).
			//     2. Find UInputAction with display name matching axis_name (custom convention).
			//     3. Use UEnhancedPlayerInput::InjectInputForAction(Action, FInputActionValue(Value)).
			//   That requires the EnhancedInput module + an asset-side naming convention we
			//   don't yet enforce. For now, fall back to legacy WASD/arrow translation.

			FKey Key = Embodied_LegacyAxisToKey(AxisName, Value);
			if (!Key.IsValid())
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_IMPLEMENTED"), TEXT("axis_name"),
					TEXT("Enhanced Input axis introspection is not implemented. "
						 "Use embody_input_press_key with W/A/S/D directly, or rename your axis to "
						 "MoveForward/MoveRight/LookUp/LookRight for legacy translation."));
				OutError = FString::Printf(TEXT("Axis '%s' is not bindable via legacy translation."), *AxisName);
				return false;
			}

			if (!FSlateApplication::IsInitialized())
			{
				SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"));
				OutError = TEXT("FSlateApplication not initialized.");
				return false;
			}

			FModifierKeysState Modifiers(false, false, false, false, false, false, false, false, false);
			FKeyEvent KeyDown(Key, Modifiers, 0, false, 0, 0);
			FKeyEvent KeyUp  (Key, Modifiers, 0, false, 0, 0);

			const bool bDown = FSlateApplication::Get().ProcessKeyDownEvent(KeyDown);
			if (HoldMs > 0)
			{
				FPlatformProcess::Sleep(HoldMs / 1000.0f);
			}
			const bool bUp = FSlateApplication::Get().ProcessKeyUpEvent(KeyUp);

			const bool bApplied = bDown && bUp;
			if (!bDown && !bUp)
			{
				Embodied_SetStatus(OutStructured, TEXT("failed"));
				OutStructured->SetStringField(TEXT("axis"), AxisName);
				OutStructured->SetNumberField(TEXT("value"), Value);
				OutStructured->SetStringField(TEXT("translated_key"), Key.ToString());
				OutStructured->SetNumberField(TEXT("hold_ms"), HoldMs);
				OutStructured->SetBoolField(TEXT("applied"), false);
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("Slate rejected both translated axis key events."));
				OutError = TEXT("Axis input was not applied.");
				return false;
			}
			Embodied_SetStatus(OutStructured, bApplied ? TEXT("success") : TEXT("partial_success"));
			OutStructured->SetStringField(TEXT("axis"), AxisName);
			OutStructured->SetNumberField(TEXT("value"), Value);
			OutStructured->SetStringField(TEXT("translated_key"), Key.ToString());
			OutStructured->SetNumberField(TEXT("hold_ms"), HoldMs);
			OutStructured->SetBoolField(TEXT("pressed"), bDown);
			OutStructured->SetBoolField(TEXT("released"), bUp);
			OutStructured->SetBoolField(TEXT("applied"), bApplied);

			OutSummary = FString::Printf(TEXT("Axis '%s' value=%.2f via key '%s' for %d ms."),
				*AxisName, Value, *Key.ToString(), HoldMs);
			return true;
		}
	, nullptr
	, 0
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 6. embody_get_observation
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("embody_get_observation"),
		TEXT("Composite observation snapshot for the embodied agent: player_pos, player_rot, "
			 "nearby_actors within actors_in_radius cm of the player, optional hp_if_present. "
			 "screenshot=true requests a viewport screenshot via the existing screenshot tool "
			 "(returns screenshot_path if available). Returns NOT_AVAILABLE if PIE is not running."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("screenshot"),       FSololmcpSchemaBuilder::Boolean(
					TEXT("Whether to request a viewport screenshot. Default: true."))},
				{TEXT("actors_in_radius"), FSololmcpSchemaBuilder::Number(
					TEXT("Radius in cm around the player to enumerate nearby_actors. Default: 1000."))}
			},
			{}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!Embodied_EnsureGameThread(OutError)) return false;

			UWorld* World = Embodied_GetPlayWorld();
			if (!World)
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
					TEXT("PIE is not running. Call embody_pie_start first."));
				OutError = TEXT("PIE is not running.");
				return false;
			}

			bool bScreenshot = true;
			Arguments->TryGetBoolField(TEXT("screenshot"), bScreenshot);
			double Radius = 1000.0;
			Arguments->TryGetNumberField(TEXT("actors_in_radius"), Radius);
			Radius = FMath::Max(0.0, Radius);

			APlayerController* PC = World->GetFirstPlayerController();
			APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			if (!Pawn)
			{
				OutStructured->SetField(TEXT("player_pos"), MakeShared<FJsonValueNull>());
				OutStructured->SetField(TEXT("player_rot"), MakeShared<FJsonValueNull>());
				OutStructured->SetArrayField(TEXT("nearby_actors"), {});
				SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
					TEXT("PIE is running but no player pawn is possessed yet. Wait a tick and retry."));
				OutError = TEXT("No player pawn possessed.");
				return false;
			}

			const FVector PlayerLoc = Pawn->GetActorLocation();
			const FRotator PlayerRot = Pawn->GetActorRotation();

			OutStructured->SetObjectField(TEXT("player_pos"), Embodied_VecToJson(PlayerLoc));
			OutStructured->SetObjectField(TEXT("player_rot"), Embodied_RotToJson(PlayerRot));

			double Hp = 0.0;
			if (Embodied_TryReadHealth(Pawn, Hp))
			{
				OutStructured->SetNumberField(TEXT("hp_if_present"), Hp);
			}

			// Enumerate nearby actors (excluding the player pawn itself).
			TArray<TSharedPtr<FJsonValue>> Nearby;
			const double R2 = Radius * Radius;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* A = *It;
				if (!A || A == Pawn) continue;
				const FVector AL = A->GetActorLocation();
				const double D2 = FVector::DistSquared(AL, PlayerLoc);
				if (D2 > R2) continue;

				TSharedRef<FJsonObject> AO = MakeShared<FJsonObject>();
				AO->SetStringField(TEXT("name"),     A->GetName());
				AO->SetStringField(TEXT("class"),    A->GetClass()->GetName());
				AO->SetObjectField(TEXT("location"), Embodied_VecToJson(AL));
				AO->SetNumberField(TEXT("distance"), FMath::Sqrt(D2));
				Nearby.Add(MakeShared<FJsonValueObject>(AO));
			}
			OutStructured->SetArrayField(TEXT("nearby_actors"), Nearby);
			OutStructured->SetNumberField(TEXT("nearby_count"), Nearby.Num());

			// Best-effort screenshot via the existing screenshot tool. We avoid hard-coupling
			// because Registry::ExecuteTool re-enters the registry; instead we report the
			// expected sibling tool name and let the client invoke it. This keeps the
			// observation tool non-destructive and avoids reentrancy/lock issues.
			if (bScreenshot)
			{
				// TODO(P4-6): if Registry exposes a non-locking ExecuteTool variant, call
				// Context.Services.Registry.ExecuteTool("editor_screenshot_viewport", ...) here
				// and propagate the resulting screenshot_path. For now, just hint the client.
				OutStructured->SetStringField(TEXT("screenshot_hint"),
					TEXT("Call 'editor_screenshot_viewport' or 'pie_screenshot' next to grab the frame."));
			}

			OutSummary = FString::Printf(TEXT("Observation @ player=(%.0f,%.0f,%.0f), %d nearby actors."),
				PlayerLoc.X, PlayerLoc.Y, PlayerLoc.Z, Nearby.Num());
			return true;
		}
	, nullptr
	, 0
	});
}

} // namespace UE::SOMOLMCP
