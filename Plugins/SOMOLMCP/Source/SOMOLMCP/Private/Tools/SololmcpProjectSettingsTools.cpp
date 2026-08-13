// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpProjectSettingsTools.cpp — SOMOLMCP P3-4
// Project / Plugin settings tools (4):
//   project_settings_get / project_settings_set / plugin_enable / plugin_disable
//
// Notes (UE 5.7):
//   - Console variables (r.* / a.* / p.* / Slate.*) are read/written through
//     IConsoleManager::Get().FindConsoleVariable(...).
//   - Typed UDeveloperSettings (URendererSettings, UPhysicsSettings, UAISystemBase, ...)
//     are reflected with GetMutableDefault<T>() + FProperty::ImportText_Direct.
//   - SetPluginEnabled moved from IPluginManager to IProjectManager in 5.x.
//   - All edits to typed settings are persisted with TryUpdateDefaultConfigFile() so they survive restart.
//

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

// Console variables
#include "HAL/IConsoleManager.h"

// Project / plugin management
// UE 5.7: IPlugin is part of IPluginManager.h (no separate header).
#include "Interfaces/IProjectManager.h"
#include "Interfaces/IPluginManager.h"

// Config / typed settings
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "Engine/RendererSettings.h"
#include "PhysicsEngine/PhysicsSettings.h"
// AISystem.h is private to AIModule but UAISystemBase is exported via Engine
#include "AISystem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/SharedPointer.h"
#include "Internationalization/Text.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPProjectSettings, Log, All);

namespace UE::SOMOLMCP
{

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool ProjectSettings_EnsureGameThread(FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("Project settings tools must be called from the game thread.");
		return false;
	}
	return true;
}

/** Resolve a category string to a typed UDeveloperSettings CDO (mutable). */
static UObject* ProjectSettings_ResolveCategoryCDO(const FString& Category)
{
	const FString C = Category.TrimStartAndEnd().ToLower();
	if (C == TEXT("renderer") || C == TEXT("rendering"))
	{
		return GetMutableDefault<URendererSettings>();
	}
	if (C == TEXT("physics"))
	{
		return GetMutableDefault<UPhysicsSettings>();
	}
	if (C == TEXT("aisystem") || C == TEXT("ai"))
	{
		// TODO(P3-4): UAISystemBase is the right CDO for runtime AI; project-level
		// AISystemSettings live in the Engine ini under [/Script/AIModule.AISystem].
		return GetMutableDefault<UAISystemBase>();
	}
	// TODO(P3-4): extend mapping (Audio, Engine, Game, Network, Editor, ...).
	return nullptr;
}

/** Stringify the current value of a typed property for response payload. */
static FString ProjectSettings_PropertyValueAsString(const FProperty* Property, const void* Container)
{
	if (!Property || !Container) return FString();
	FString Out;
	Property->ExportTextItem_Direct(Out, Property->ContainerPtrToValuePtr<void>(Container), nullptr, nullptr, PPF_None);
	return Out;
}

/** Stringify any JSON value into the canonical UE textual form expected by ImportText_Direct / CVar->Set. */
static FString ProjectSettings_JsonValueToString(const TSharedPtr<FJsonValue>& Val)
{
	if (!Val.IsValid()) return FString();
	switch (Val->Type)
	{
		case EJson::String:  return Val->AsString();
		case EJson::Number:  return FString::SanitizeFloat(Val->AsNumber());
		case EJson::Boolean: return Val->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Null:    return TEXT("");
		default:
		{
			FString Out;
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Val.ToSharedRef(), TEXT(""), W);
			return Out;
		}
	}
}

/** Best-effort guess: does setting this property require an editor restart? */
static bool ProjectSettings_GuessRequiresRestart(const UObject* Cdo, const FProperty* Property)
{
	if (!Cdo || !Property) return false;
	// Properties with Config + (GlobalConfig || RestartRequired meta) tend to need a restart.
	if (Property->HasAnyPropertyFlags(CPF_GlobalConfig)) return true;
	if (Property->HasMetaData(TEXT("ConfigRestartRequired"))) return true;
	// Renderer settings dealing with shader permutations almost always require restart.
	if (Cdo->IsA(URendererSettings::StaticClass())
		&& Property->GetFName().ToString().StartsWith(TEXT("b"), ESearchCase::CaseSensitive))
	{
		// Heuristic only — refined per-property as needed.
		// TODO(P3-4): build an explicit restart-required allowlist for renderer settings.
	}
	return false;
}

/**
 * CVars in this set change renderer feature layouts, shader permutations, or
 * RHI resource formats. Applying them to a live editor is unsafe (r.Substrate
 * can invalidate material/render state while it is in use), so they are only
 * persisted and are never passed to IConsoleVariable::Set.
 */
static bool ProjectSettings_IsRestartOnlyCVar(const FString& SettingName)
{
	static const TCHAR* RestartOnlyPrefixes[] =
	{
		TEXT("r.Substrate"),
		TEXT("r.ForwardShading"),
		TEXT("r.Mobile.ShadingPath"),
		TEXT("r.AllowStaticLighting"),
		TEXT("r.SkinCache.CompileShaders"),
		TEXT("r.RayTracing"),
		TEXT("r.VirtualTextures"),
		TEXT("r.Nanite.ProjectEnabled"),
		TEXT("r.Lumen.HardwareRayTracing")
	};
	for (const TCHAR* Prefix : RestartOnlyPrefixes)
	{
		if (SettingName.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

static bool ProjectSettings_WriteCVarConfigOnly(
	const FString& SettingName,
	const FString& NewValue,
	FString& OutConfigPath,
	FString& OutConfigSection,
	FString& OutError)
{
	if (!GConfig)
	{
		OutError = TEXT("Unreal config cache is unavailable.");
		return false;
	}
	OutConfigPath = FPaths::SourceConfigDir() / TEXT("DefaultEngine.ini");
	// Project renderer switches are consumed by URendererSettings from this
	// section. Non-renderer console variables retain the standard SystemSettings
	// persistence route.
	OutConfigSection = SettingName.StartsWith(TEXT("r."), ESearchCase::IgnoreCase)
		? TEXT("/Script/Engine.RendererSettings")
		: TEXT("SystemSettings");
	GConfig->SetString(*OutConfigSection, *SettingName, *NewValue, OutConfigPath);
	GConfig->Flush(false, OutConfigPath);

	FString PersistedValue;
	if (!GConfig->GetString(*OutConfigSection, *SettingName, PersistedValue, OutConfigPath)
		|| PersistedValue != NewValue)
	{
		OutError = FString::Printf(TEXT("Failed to verify persisted CVar value in %s."), *OutConfigPath);
		return false;
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// RegisterProjectSettingsTools
// ─────────────────────────────────────────────────────────────────────────────

void RegisterProjectSettingsTools(FSololmcpToolRegistry& Registry)
{
	// ─────────────────────────────────────────────────────────────────────────
	// 1. project_settings_get
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("project_settings_get"),
		TEXT("Read a project setting. For console variables (e.g. 'r.SkyAtmosphere'), pass any "
			 "category — the name itself is sufficient. For typed UDeveloperSettings, pass "
			 "category in {Renderer, Physics, AISystem} and setting_name as the property name."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("category"),     FSololmcpSchemaBuilder::String(TEXT("Settings category: Renderer | Physics | AISystem | CVar."))},
				{TEXT("setting_name"), FSololmcpSchemaBuilder::String(TEXT("Property name (e.g. 'DefaultClassesToInstance') or CVar name (e.g. 'r.SkyAtmosphere')."))}
			},
			{TEXT("category"), TEXT("setting_name")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!ProjectSettings_EnsureGameThread(OutError)) return false;

			FString Category, SettingName;
			if (!Arguments->TryGetStringField(TEXT("category"), Category) || Category.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("category"));
				OutError = TEXT("Missing required argument: category");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("setting_name"), SettingName) || SettingName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("setting_name"));
				OutError = TEXT("Missing required argument: setting_name");
				return false;
			}

			// First try CVars: r.* / a.* / p.* / Slate.* / etc., or anything explicitly tagged "CVar".
			const bool bLooksLikeCVar = SettingName.Contains(TEXT("."))
				|| Category.Equals(TEXT("CVar"), ESearchCase::IgnoreCase);
			if (bLooksLikeCVar)
			{
				IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*SettingName);
				if (CVar)
				{
					OutStructured->SetStringField(TEXT("category"),  TEXT("CVar"));
					OutStructured->SetStringField(TEXT("setting"),   SettingName);
					OutStructured->SetStringField(TEXT("value"),     CVar->GetString());
					OutStructured->SetStringField(TEXT("type"),      TEXT("cvar"));
					OutSummary = FString::Printf(TEXT("CVar %s = %s"), *SettingName, *CVar->GetString());
					return true;
				}
				if (Category.Equals(TEXT("CVar"), ESearchCase::IgnoreCase))
				{
					SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("CVar '%s'"), *SettingName));
					OutError = FString::Printf(TEXT("Console variable '%s' not found."), *SettingName);
					return false;
				}
				// Fall through to typed settings if name happened to contain a dot.
			}

			// Typed UDeveloperSettings.
			UObject* Cdo = ProjectSettings_ResolveCategoryCDO(Category);
			if (!Cdo)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("category '%s'"), *Category));
				OutError = FString::Printf(TEXT("Unsupported category '%s'. Use Renderer | Physics | AISystem | CVar."), *Category);
				return false;
			}
			FProperty* Prop = Cdo->GetClass()->FindPropertyByName(*SettingName);
			if (!Prop)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("property '%s.%s'"), *Category, *SettingName));
				OutError = FString::Printf(TEXT("Property '%s' not found on %s."), *SettingName, *Cdo->GetClass()->GetName());
				return false;
			}

			OutStructured->SetStringField(TEXT("category"), Category);
			OutStructured->SetStringField(TEXT("setting"),  SettingName);
			OutStructured->SetStringField(TEXT("value"),    ProjectSettings_PropertyValueAsString(Prop, Cdo));
			OutStructured->SetStringField(TEXT("type"),     Prop->GetCPPType());

			OutSummary = FString::Printf(TEXT("%s.%s read."), *Category, *SettingName);
			return true;
		}
	, nullptr
	, 5
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 2. project_settings_set
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("project_settings_set"),
		TEXT("Write a project setting. Mirrors project_settings_get; persists typed settings "
			 "via TryUpdateDefaultConfigFile(). Renderer-layout/shader CVars such as r.Substrate "
			 "are config-only and never hot-applied. Returns previous and new values plus a "
			 "requires_restart hint."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("category"),     FSololmcpSchemaBuilder::String(TEXT("Settings category: Renderer | Physics | AISystem | CVar."))},
				{TEXT("setting_name"), FSololmcpSchemaBuilder::String(TEXT("Property or CVar name."))},
				{TEXT("value"),        FSololmcpSchemaBuilder::String(TEXT("New value. Strings/numbers/bools accepted; serialized via ImportText / CVar->Set."))},
				{TEXT("apply_mode"),   FSololmcpSchemaBuilder::String(TEXT("auto (default), runtime, or config_only. Restart-only CVars always force config_only."), {TEXT("auto"), TEXT("runtime"), TEXT("config_only")})}
			},
			{TEXT("category"), TEXT("setting_name"), TEXT("value")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!ProjectSettings_EnsureGameThread(OutError)) return false;

			FString Category, SettingName;
			if (!Arguments->TryGetStringField(TEXT("category"), Category) || Category.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("category"));
				OutError = TEXT("Missing required argument: category");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("setting_name"), SettingName) || SettingName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("setting_name"));
				OutError = TEXT("Missing required argument: setting_name");
				return false;
			}
			TSharedPtr<FJsonValue> ValField = Arguments->TryGetField(TEXT("value"));
			if (!ValField.IsValid())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("value"));
				OutError = TEXT("Missing required argument: value");
				return false;
			}
			const FString NewValueStr = ProjectSettings_JsonValueToString(ValField);
			FString ApplyMode = TEXT("auto");
			Arguments->TryGetStringField(TEXT("apply_mode"), ApplyMode);
			ApplyMode = ApplyMode.TrimStartAndEnd().ToLower();
			if (ApplyMode != TEXT("auto") && ApplyMode != TEXT("runtime") && ApplyMode != TEXT("config_only"))
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_ARGUMENT"), TEXT("apply_mode"),
					TEXT("Use auto, runtime, or config_only."));
				OutError = TEXT("Invalid apply_mode.");
				return false;
			}

			// Path A: console variable.
			const bool bLooksLikeCVar = SettingName.Contains(TEXT("."))
				|| Category.Equals(TEXT("CVar"), ESearchCase::IgnoreCase);
			if (bLooksLikeCVar)
			{
				IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*SettingName);
				if (CVar)
				{
					const FString OldValue = CVar->GetString();
					const bool bRestartOnly = ProjectSettings_IsRestartOnlyCVar(SettingName);
					const bool bConfigOnly = bRestartOnly || ApplyMode == TEXT("config_only");
					if (bConfigOnly)
					{
						FString ConfigPath;
						FString ConfigSection;
						if (!ProjectSettings_WriteCVarConfigOnly(SettingName, NewValueStr, ConfigPath, ConfigSection, OutError))
						{
							SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("value"), OutError);
							return false;
						}
						OutStructured->SetStringField(TEXT("category"), TEXT("CVar"));
						OutStructured->SetStringField(TEXT("setting"), SettingName);
						OutStructured->SetStringField(TEXT("old_runtime_value"), OldValue);
						OutStructured->SetStringField(TEXT("configured_value"), NewValueStr);
						OutStructured->SetStringField(TEXT("runtime_value"), CVar->GetString());
						OutStructured->SetStringField(TEXT("apply_mode"), TEXT("config_only"));
						OutStructured->SetStringField(TEXT("config_file"), ConfigPath);
						OutStructured->SetStringField(TEXT("config_section"), ConfigSection);
						OutStructured->SetBoolField(TEXT("hot_applied"), false);
						OutStructured->SetBoolField(TEXT("requires_restart"), true);
						OutSummary = FString::Printf(TEXT("CVar %s configured as '%s' (restart required; runtime unchanged)."),
							*SettingName, *NewValueStr);
						return true;
					}
					CVar->Set(*NewValueStr, ECVF_SetByCode);
					const FString FreshValue = CVar->GetString();
					if (FreshValue == OldValue)
					{
						SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("value"),
							TEXT("CVar value did not change after Set()."));
						OutError = FString::Printf(TEXT("Console variable '%s' remained '%s' after Set('%s')."),
							*SettingName, *OldValue, *NewValueStr);
						return false;
					}

					OutStructured->SetStringField(TEXT("category"),         TEXT("CVar"));
					OutStructured->SetStringField(TEXT("setting"),          SettingName);
					OutStructured->SetStringField(TEXT("old_value"),        OldValue);
					OutStructured->SetStringField(TEXT("new_value"),        FreshValue);
					OutStructured->SetStringField(TEXT("apply_mode"),       TEXT("runtime"));
					OutStructured->SetBoolField  (TEXT("hot_applied"),      true);
					OutStructured->SetBoolField  (TEXT("requires_restart"), false);
					OutSummary = FString::Printf(TEXT("CVar %s: '%s' -> '%s'"), *SettingName, *OldValue, *FreshValue);
					return true;
				}
				if (Category.Equals(TEXT("CVar"), ESearchCase::IgnoreCase))
				{
					SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("CVar '%s'"), *SettingName));
					OutError = FString::Printf(TEXT("Console variable '%s' not found."), *SettingName);
					return false;
				}
				// Fall through if name had a dot but isn't a CVar.
			}

			// Path B: typed UDeveloperSettings.
			UObject* Cdo = ProjectSettings_ResolveCategoryCDO(Category);
			if (!Cdo)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("category '%s'"), *Category));
				OutError = FString::Printf(TEXT("Unsupported category '%s'."), *Category);
				return false;
			}
			FProperty* Prop = Cdo->GetClass()->FindPropertyByName(*SettingName);
			if (!Prop)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("property '%s.%s'"), *Category, *SettingName));
				OutError = FString::Printf(TEXT("Property '%s' not found on %s."), *SettingName, *Cdo->GetClass()->GetName());
				return false;
			}
			if (!Prop->HasAnyPropertyFlags(CPF_Config | CPF_GlobalConfig))
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("setting_name"),
					TEXT("Property is not a config property and cannot be persisted as a project setting."));
				OutError = FString::Printf(TEXT("Property '%s' on %s is not marked Config/GlobalConfig."),
					*SettingName, *Cdo->GetClass()->GetName());
				return false;
			}

			const FString OldValue = ProjectSettings_PropertyValueAsString(Prop, Cdo);

			// ImportText_Direct writes into the property's storage for the given container.
			const TCHAR* ImportPtr = *NewValueStr;
			Cdo->Modify();
			const TCHAR* RemainingText = Prop->ImportText_Direct(
				ImportPtr,
				Prop->ContainerPtrToValuePtr<void>(Cdo),
				Cdo,
				PPF_None);
			if (RemainingText == nullptr)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("value"),
					FString::Printf(TEXT("ImportText_Direct rejected '%s' for property type '%s'."),
						*NewValueStr, *Prop->GetCPPType()));
				OutError = FString::Printf(TEXT("Failed to assign value '%s' to %s.%s."), *NewValueStr, *Category, *SettingName);
				return false;
			}
			if (FCString::Strlen(RemainingText) > 0 && !FString(RemainingText).TrimStartAndEnd().IsEmpty())
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("value"),
					FString::Printf(TEXT("ImportText_Direct left unparsed text '%s'."), RemainingText));
				OutError = FString::Printf(TEXT("Value '%s' was only partially parsed for %s.%s."),
					*NewValueStr, *Category, *SettingName);
				return false;
			}

			const FString FreshValue = ProjectSettings_PropertyValueAsString(Prop, Cdo);
			if (FreshValue == OldValue)
			{
				SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("value"),
					TEXT("Property value did not change after ImportText_Direct."));
				OutError = FString::Printf(TEXT("%s.%s remained '%s' after assigning '%s'."),
					*Category, *SettingName, *OldValue, *NewValueStr);
				return false;
			}

			if (!Cdo->TryUpdateDefaultConfigFile())
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("setting_name"),
					TEXT("Failed to write the settings object's default config file."));
				OutError = FString::Printf(TEXT("Failed to save config for %s after changing %s.%s."),
					*Cdo->GetClass()->GetName(), *Category, *SettingName);
				return false;
			}
			const bool bNeedsRestart = ProjectSettings_GuessRequiresRestart(Cdo, Prop);

			OutStructured->SetStringField(TEXT("category"),         Category);
			OutStructured->SetStringField(TEXT("setting"),          SettingName);
			OutStructured->SetStringField(TEXT("old_value"),        OldValue);
			OutStructured->SetStringField(TEXT("new_value"),        FreshValue);
			OutStructured->SetBoolField  (TEXT("requires_restart"), bNeedsRestart);

			OutSummary = FString::Printf(TEXT("%s.%s: '%s' -> '%s'%s"),
				*Category, *SettingName, *OldValue, *FreshValue,
				bNeedsRestart ? TEXT(" (restart required)") : TEXT(""));
			return true;
		}
	, nullptr
	, 0
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 3. plugin_enable
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("plugin_enable"),
		TEXT("Enable a project plugin (writes to .uproject). Editor restart is required for "
			 "the change to take effect for module-loading plugins."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("plugin_name"), FSololmcpSchemaBuilder::String(TEXT("Plugin name (e.g. 'PCG', 'ChaosCloth', 'Substrate')."))}
			},
			{TEXT("plugin_name")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!ProjectSettings_EnsureGameThread(OutError)) return false;

			FString PluginName;
			if (!Arguments->TryGetStringField(TEXT("plugin_name"), PluginName) || PluginName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("plugin_name"));
				OutError = TEXT("Missing required argument: plugin_name");
				return false;
			}

			TSharedPtr<IPlugin> Plug = IPluginManager::Get().FindPlugin(PluginName);
			if (!Plug.IsValid())
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("plugin '%s'"), *PluginName));
				OutError = FString::Printf(TEXT("Plugin '%s' not discovered."), *PluginName);
				return false;
			}
			if (Plug->IsEnabled())
			{
				SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("plugin_name"),
					TEXT("Plugin is already enabled."));
				OutError = FString::Printf(TEXT("Plugin '%s' is already enabled."), *PluginName);
				return false;
			}

			FText FailReason;
			// UE 5.7: SetPluginEnabled lives on IProjectManager, not IPluginManager.
			const bool bOk = IProjectManager::Get().SetPluginEnabled(PluginName, /*bEnabled=*/true, FailReason);
			if (!bOk)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("plugin_name"),
					FailReason.ToString());
				OutError = FString::Printf(TEXT("Enable failed: %s"), *FailReason.ToString());
				return false;
			}
			FText SaveFailReason;
			if (!IProjectManager::Get().SaveCurrentProjectToDisk(SaveFailReason))
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("plugin_name"),
					SaveFailReason.ToString());
				OutError = FString::Printf(TEXT("Enable changed project descriptor but failed to save .uproject: %s"),
					*SaveFailReason.ToString());
				return false;
			}
			TSharedPtr<IPlugin> FreshPlug = IPluginManager::Get().FindPlugin(PluginName);
			const bool bRuntimeEnabled = FreshPlug.IsValid() && FreshPlug->IsEnabled();

			OutStructured->SetStringField(TEXT("plugin"),           PluginName);
			OutStructured->SetBoolField  (TEXT("enabled"),          true);
			OutStructured->SetBoolField  (TEXT("project_file_saved"), true);
			OutStructured->SetBoolField  (TEXT("runtime_enabled"),  bRuntimeEnabled);
			OutStructured->SetBoolField  (TEXT("requires_restart"), true);
			OutSummary = FString::Printf(TEXT("Enabled plugin '%s' (restart required)."), *PluginName);
			return true;
		}
	, nullptr
	, 0
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 4. plugin_disable
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("plugin_disable"),
		TEXT("Disable a project plugin (writes to .uproject). Editor restart is required."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("plugin_name"), FSololmcpSchemaBuilder::String(TEXT("Plugin name to disable."))}
			},
			{TEXT("plugin_name")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!ProjectSettings_EnsureGameThread(OutError)) return false;

			FString PluginName;
			if (!Arguments->TryGetStringField(TEXT("plugin_name"), PluginName) || PluginName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("plugin_name"));
				OutError = TEXT("Missing required argument: plugin_name");
				return false;
			}

			TSharedPtr<IPlugin> Plug = IPluginManager::Get().FindPlugin(PluginName);
			if (!Plug.IsValid())
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("plugin '%s'"), *PluginName));
				OutError = FString::Printf(TEXT("Plugin '%s' not discovered."), *PluginName);
				return false;
			}
			if (!Plug->IsEnabled())
			{
				SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("plugin_name"),
					TEXT("Plugin is already disabled."));
				OutError = FString::Printf(TEXT("Plugin '%s' is already disabled."), *PluginName);
				return false;
			}

			FText FailReason;
			const bool bOk = IProjectManager::Get().SetPluginEnabled(PluginName, /*bEnabled=*/false, FailReason);
			if (!bOk)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("plugin_name"),
					FailReason.ToString());
				OutError = FString::Printf(TEXT("Disable failed: %s"), *FailReason.ToString());
				return false;
			}
			FText SaveFailReason;
			if (!IProjectManager::Get().SaveCurrentProjectToDisk(SaveFailReason))
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("plugin_name"),
					SaveFailReason.ToString());
				OutError = FString::Printf(TEXT("Disable changed project descriptor but failed to save .uproject: %s"),
					*SaveFailReason.ToString());
				return false;
			}
			TSharedPtr<IPlugin> FreshPlug = IPluginManager::Get().FindPlugin(PluginName);
			const bool bRuntimeEnabled = FreshPlug.IsValid() && FreshPlug->IsEnabled();

			OutStructured->SetStringField(TEXT("plugin"),           PluginName);
			OutStructured->SetBoolField  (TEXT("enabled"),          false);
			OutStructured->SetBoolField  (TEXT("project_file_saved"), true);
			OutStructured->SetBoolField  (TEXT("runtime_enabled"),  bRuntimeEnabled);
			OutStructured->SetBoolField  (TEXT("requires_restart"), true);
			OutSummary = FString::Printf(TEXT("Disabled plugin '%s' (restart required)."), *PluginName);
			return true;
		}
	, nullptr
	, 0
	});
}

} // namespace UE::SOMOLMCP
