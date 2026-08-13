// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpPluginDiscoveryTools.cpp  (P5)
// ----------------------------------------------------------------------------
// Plugin discovery / inspection tools for the SOMOLMCP UE plugin.
//
// Four tools registered via RegisterPluginDiscoveryTools(FSololmcpToolRegistry&):
//
//   plugin_list_all             — All discovered plugins, optional category filter.
//   plugin_inspect              — Single-plugin descriptor (modules, deps, etc.).
//   plugin_check_compatibility  — Compare plugin engine version vs ENGINE_VERSION_STRING.
//   plugin_recommend_for_role   — Hardcoded role-to-plugin matrix (foliage_author, etc.).
//
// NOTE: plugin_enable / plugin_disable already exist in P3-4
// (SololmcpProjectSettingsTools.cpp). They are NOT re-registered here.
//
// HARD CONSTRAINTS honored:
//   - Does NOT modify SololmcpToolRegistry.{h,cpp}.
//   - Does NOT modify .Build.cs (audit only — see P5_DELIVERY.md).
//   - Does NOT modify any other .cpp file.
//
// File location for integration:
// then add a forward declaration of RegisterPluginDiscoveryTools() to
// SololmcpToolRegistry.h and a call site in SololmcpToolRegistry.cpp's constructor.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Interfaces/IPluginManager.h"
// UE 5.7: IPlugin lives inside IPluginManager.h — no separate header.
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"
#include "PluginDescriptor.h"

#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"

#include "Templates/SharedPointer.h"

namespace UE::SOMOLMCP
{
namespace PluginDiscoveryImpl
{
	// ------------------------------------------------------------------
	// Helpers
	// ------------------------------------------------------------------

	/** Map IPlugin::GetLoadedFrom to a stable short category. */
	static FString CategoryForPlugin(const TSharedRef<IPlugin>& Plug)
	{
		switch (Plug->GetLoadedFrom())
		{
			case EPluginLoadedFrom::Engine:  return TEXT("engine");
			case EPluginLoadedFrom::Project: return TEXT("project");
			default:                          return TEXT("project");
		}
	}

	/** Marketplace plugins live under <Engine>/Plugins/Marketplace or their
	 *  descriptor's MarketplaceURL is non-empty. We use the latter as a
	 *  reliable signal and the path-prefix as a secondary signal. */
	static bool IsMarketplacePlugin(const TSharedRef<IPlugin>& Plug)
	{
		const FPluginDescriptor& Desc = Plug->GetDescriptor();
		if (!Desc.MarketplaceURL.IsEmpty())
		{
			return true;
		}
		const FString BaseDir = Plug->GetBaseDir().Replace(TEXT("\\"), TEXT("/"));
		if (BaseDir.Contains(TEXT("/Plugins/Marketplace/")))
		{
			return true;
		}
		return false;
	}

	static FString EffectiveCategory(const TSharedRef<IPlugin>& Plug)
	{
		if (IsMarketplacePlugin(Plug)) return TEXT("marketplace");
		return CategoryForPlugin(Plug);
	}

	static TSharedRef<FJsonObject> PluginToSummaryJson(const TSharedRef<IPlugin>& Plug)
	{
		const FPluginDescriptor& Desc = Plug->GetDescriptor();
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"),        Plug->GetName());
		Obj->SetStringField(TEXT("version"),     Desc.VersionName);
		Obj->SetBoolField  (TEXT("enabled"),     Plug->IsEnabled());
		Obj->SetStringField(TEXT("category"),    EffectiveCategory(Plug));
		Obj->SetStringField(TEXT("description"), Desc.Description);
		Obj->SetStringField(TEXT("author"),      Desc.CreatedBy);
		return Obj;
	}

	// ------------------------------------------------------------------
	// 1) plugin_list_all
	// ------------------------------------------------------------------
	static bool RunListAll(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& /*OutError*/)
	{
		FString Filter;
		Arguments->TryGetStringField(TEXT("category_filter"), Filter);
		Filter = Filter.ToLower();

		const TArray<TSharedRef<IPlugin>> All = IPluginManager::Get().GetDiscoveredPlugins();

		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(All.Num());
		for (const TSharedRef<IPlugin>& Plug : All)
		{
			const FString Cat = EffectiveCategory(Plug);
			if (!Filter.IsEmpty() && Cat != Filter)
			{
				continue;
			}
			Out.Add(MakeShared<FJsonValueObject>(PluginToSummaryJson(Plug)));
		}

		// Sort by name asc for stable output.
		Out.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsObject()->GetStringField(TEXT("name"))
			     < B->AsObject()->GetStringField(TEXT("name"));
		});

		OutStructured->SetNumberField(TEXT("count"), Out.Num());
		OutStructured->SetArrayField(TEXT("plugins"), Out);
		if (!Filter.IsEmpty())
		{
			OutStructured->SetStringField(TEXT("category_filter"), Filter);
		}
		OutSummary = FString::Printf(TEXT("Discovered %d plugins%s."),
			Out.Num(),
			Filter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (filter=%s)"), *Filter));
		return true;
	}

	// ------------------------------------------------------------------
	// 2) plugin_inspect
	// ------------------------------------------------------------------
	static bool RunInspect(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString PluginName;
		if (!Arguments->TryGetStringField(TEXT("plugin_name"), PluginName) || PluginName.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("plugin_name"));
			OutError = TEXT("Missing plugin_name.");
			return false;
		}

		TSharedPtr<IPlugin> Plug = IPluginManager::Get().FindPlugin(PluginName);
		if (!Plug.IsValid())
		{
			SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("plugin '%s'"), *PluginName));
			OutError = FString::Printf(TEXT("Plugin '%s' not discovered."), *PluginName);
			return false;
		}

		const FPluginDescriptor& Desc = Plug->GetDescriptor();

		OutStructured->SetStringField(TEXT("name"),        Plug->GetName());
		OutStructured->SetStringField(TEXT("version"),     Desc.VersionName);
		OutStructured->SetBoolField  (TEXT("enabled"),     Plug.ToSharedRef()->IsEnabled());
		OutStructured->SetStringField(TEXT("category"),    EffectiveCategory(Plug.ToSharedRef()));
		OutStructured->SetStringField(TEXT("description"), Desc.Description);
		OutStructured->SetStringField(TEXT("author"),      Desc.CreatedBy);
		OutStructured->SetStringField(TEXT("base_dir"),    Plug->GetBaseDir());
		OutStructured->SetStringField(TEXT("descriptor_file"), Plug->GetDescriptorFileName());
		OutStructured->SetBoolField  (TEXT("content_only"), Desc.bIsPluginExtension ? false : Desc.Modules.IsEmpty());

		// Modules array.
		TArray<TSharedPtr<FJsonValue>> ModulesJson;
		for (const FModuleDescriptor& M : Desc.Modules)
		{
			TSharedRef<FJsonObject> ModObj = MakeShared<FJsonObject>();
			ModObj->SetStringField(TEXT("name"), M.Name.ToString());
			// Type enum -> string
			FString TypeStr;
			switch (M.Type)
			{
				case EHostType::Runtime:                TypeStr = TEXT("Runtime"); break;
				case EHostType::RuntimeNoCommandlet:    TypeStr = TEXT("RuntimeNoCommandlet"); break;
				case EHostType::RuntimeAndProgram:      TypeStr = TEXT("RuntimeAndProgram"); break;
				case EHostType::CookedOnly:             TypeStr = TEXT("CookedOnly"); break;
				case EHostType::UncookedOnly:           TypeStr = TEXT("UncookedOnly"); break;
				case EHostType::Developer:              TypeStr = TEXT("Developer"); break;
				case EHostType::DeveloperTool:          TypeStr = TEXT("DeveloperTool"); break;
				case EHostType::Editor:                 TypeStr = TEXT("Editor"); break;
				case EHostType::EditorNoCommandlet:     TypeStr = TEXT("EditorNoCommandlet"); break;
				case EHostType::EditorAndProgram:       TypeStr = TEXT("EditorAndProgram"); break;
				case EHostType::Program:                TypeStr = TEXT("Program"); break;
				case EHostType::ServerOnly:             TypeStr = TEXT("ServerOnly"); break;
				case EHostType::ClientOnly:             TypeStr = TEXT("ClientOnly"); break;
				case EHostType::ClientOnlyNoCommandlet: TypeStr = TEXT("ClientOnlyNoCommandlet"); break;
				default:                                 TypeStr = TEXT("Unknown"); break;
			}
			ModObj->SetStringField(TEXT("type"), TypeStr);
			FString PhaseStr;
			switch (M.LoadingPhase)
			{
				case ELoadingPhase::EarliestPossible:               PhaseStr = TEXT("EarliestPossible"); break;
				case ELoadingPhase::PostConfigInit:                 PhaseStr = TEXT("PostConfigInit"); break;
				case ELoadingPhase::PostSplashScreen:               PhaseStr = TEXT("PostSplashScreen"); break;
				case ELoadingPhase::PreEarlyLoadingScreen:          PhaseStr = TEXT("PreEarlyLoadingScreen"); break;
				case ELoadingPhase::PreLoadingScreen:               PhaseStr = TEXT("PreLoadingScreen"); break;
				case ELoadingPhase::PreDefault:                     PhaseStr = TEXT("PreDefault"); break;
				case ELoadingPhase::Default:                        PhaseStr = TEXT("Default"); break;
				case ELoadingPhase::PostDefault:                    PhaseStr = TEXT("PostDefault"); break;
				case ELoadingPhase::PostEngineInit:                 PhaseStr = TEXT("PostEngineInit"); break;
				case ELoadingPhase::None:                           PhaseStr = TEXT("None"); break;
				default:                                             PhaseStr = TEXT("Unknown"); break;
			}
			ModObj->SetStringField(TEXT("loading_phase"), PhaseStr);
			ModulesJson.Add(MakeShared<FJsonValueObject>(ModObj));
		}
		OutStructured->SetArrayField(TEXT("modules"), ModulesJson);

		// Dependencies array.
		TArray<TSharedPtr<FJsonValue>> DepsJson;
		for (const FPluginReferenceDescriptor& Ref : Desc.Plugins)
		{
			TSharedRef<FJsonObject> DepObj = MakeShared<FJsonObject>();
			DepObj->SetStringField(TEXT("name"),    Ref.Name);
			DepObj->SetBoolField  (TEXT("enabled"), Ref.bEnabled);
			DepObj->SetBoolField  (TEXT("optional"), Ref.bOptional);
			DepsJson.Add(MakeShared<FJsonValueObject>(DepObj));
		}
		OutStructured->SetArrayField(TEXT("dependencies"), DepsJson);

		// Required engine version (descriptor field is EngineVersion).
		OutStructured->SetStringField(TEXT("required_engine_version"), Desc.EngineVersion);

		OutSummary = FString::Printf(TEXT("Plugin '%s' v%s — %d modules, %d deps."),
			*PluginName, *Desc.VersionName, Desc.Modules.Num(), Desc.Plugins.Num());
		return true;
	}

	// ------------------------------------------------------------------
	// 3) plugin_check_compatibility
	// ------------------------------------------------------------------
	static bool RunCheckCompatibility(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString PluginName;
		if (!Arguments->TryGetStringField(TEXT("plugin_name"), PluginName) || PluginName.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("plugin_name"));
			OutError = TEXT("Missing plugin_name.");
			return false;
		}

		TSharedPtr<IPlugin> Plug = IPluginManager::Get().FindPlugin(PluginName);
		if (!Plug.IsValid())
		{
			SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("plugin '%s'"), *PluginName));
			OutError = FString::Printf(TEXT("Plugin '%s' not discovered."), *PluginName);
			return false;
		}

		const FPluginDescriptor& Desc = Plug->GetDescriptor();
		const FString CurrentEngine = ENGINE_VERSION_STRING;

		TArray<FString> Issues;
		bool bCompatible = true;

		// EngineVersion is often empty for project plugins (= compatible by default).
		if (Desc.EngineVersion.IsEmpty())
		{
			// no version requirement declared → assume compatible
		}
		else
		{
			// Parse "5.7.0-..." → major.minor numeric compare.
			auto ExtractMajorMinor = [](const FString& V, int32& Maj, int32& Min) -> bool
			{
				Maj = -1; Min = -1;
				FString Trim = V;
				int32 DashIdx;
				if (Trim.FindChar(TEXT('-'), DashIdx))
				{
					Trim = Trim.Left(DashIdx);
				}
				TArray<FString> Parts;
				Trim.ParseIntoArray(Parts, TEXT("."));
				if (Parts.Num() < 2) return false;
				Maj = FCString::Atoi(*Parts[0]);
				Min = FCString::Atoi(*Parts[1]);
				return Maj >= 0 && Min >= 0;
			};

			int32 ReqMaj, ReqMin, CurMaj, CurMin;
			const bool bReqOk = ExtractMajorMinor(Desc.EngineVersion, ReqMaj, ReqMin);
			const bool bCurOk = ExtractMajorMinor(CurrentEngine, CurMaj, CurMin);

			if (!bReqOk)
			{
				Issues.Add(FString::Printf(TEXT("Could not parse plugin EngineVersion '%s'."), *Desc.EngineVersion));
			}
			if (!bCurOk)
			{
				Issues.Add(FString::Printf(TEXT("Could not parse engine ENGINE_VERSION_STRING '%s'."), *CurrentEngine));
			}
			if (bReqOk && bCurOk)
			{
				if (CurMaj < ReqMaj || (CurMaj == ReqMaj && CurMin < ReqMin))
				{
					bCompatible = false;
					Issues.Add(FString::Printf(
						TEXT("Plugin requires engine >= %d.%d but running %d.%d."),
						ReqMaj, ReqMin, CurMaj, CurMin));
				}
				else if (CurMaj != ReqMaj)
				{
					Issues.Add(FString::Printf(
						TEXT("Plugin built against UE %d.%d; running on UE %d.%d (different major) — verify compatibility."),
						ReqMaj, ReqMin, CurMaj, CurMin));
				}
			}
		}

		// Marketplace plugins flagged for re-validation.
		if (IsMarketplacePlugin(Plug.ToSharedRef()))
		{
			Issues.Add(TEXT("Marketplace plugin — confirm it was repackaged for the running engine version."));
		}

		// Disabled deps aren't a compat issue per se, but worth noting.
		for (const FPluginReferenceDescriptor& Ref : Desc.Plugins)
		{
			if (!Ref.bOptional)
			{
				TSharedPtr<IPlugin> DepPlug = IPluginManager::Get().FindPlugin(Ref.Name);
				if (!DepPlug.IsValid())
				{
					Issues.Add(FString::Printf(TEXT("Required dependency '%s' is not installed."), *Ref.Name));
					bCompatible = false;
				}
				else if (!DepPlug->IsEnabled())
				{
					Issues.Add(FString::Printf(TEXT("Required dependency '%s' is installed but disabled."), *Ref.Name));
				}
			}
		}

		OutStructured->SetBoolField  (TEXT("compatible"),              bCompatible);
		OutStructured->SetStringField(TEXT("plugin"),                  PluginName);
		OutStructured->SetStringField(TEXT("required_engine_version"), Desc.EngineVersion);
		OutStructured->SetStringField(TEXT("current_engine_version"),  CurrentEngine);

		TArray<TSharedPtr<FJsonValue>> IssuesJson;
		for (const FString& I : Issues)
		{
			IssuesJson.Add(MakeShared<FJsonValueString>(I));
		}
		OutStructured->SetArrayField(TEXT("issues"), IssuesJson);

		OutSummary = FString::Printf(TEXT("Compat check '%s' vs %s: %s (%d issues)."),
			*PluginName, *CurrentEngine, bCompatible ? TEXT("OK") : TEXT("INCOMPATIBLE"), Issues.Num());
		return true;
	}

	// ------------------------------------------------------------------
	// 4) plugin_recommend_for_role
	// ------------------------------------------------------------------
	struct FRoleRecommendation
	{
		FString PluginName;
		FString Reason;
		bool    bMustHave = true;
	};

	static TArray<FRoleRecommendation> GetRecommendations(const FString& Role)
	{
		const FString R = Role.ToLower();

		if (R == TEXT("foliage_author"))
		{
			return {
				{TEXT("PCG"),               TEXT("Procedural Content Generation graph for runtime / authoring scatter."), true},
				{TEXT("ProceduralFoliage"), TEXT("Foliage spawner volumes and species config."),                          true},
			};
		}
		if (R == TEXT("animation_author"))
		{
			return {
				{TEXT("ControlRig"), TEXT("Procedural rigging and animation in-editor."), true},
				{TEXT("IKRig"),      TEXT("Skeleton retargeting and IK chains."),         true},
			};
		}
		if (R == TEXT("material_author"))
		{
			return {
				{TEXT("Substrate"), TEXT("Layered material system (UE 5.5+ preview API)."), true},
			};
		}
		if (R == TEXT("vfx_author"))
		{
			return {
				{TEXT("Niagara"), TEXT("GPU and CPU particle systems."), true},
			};
		}
		if (R == TEXT("audio_author"))
		{
			return {
				{TEXT("MetaSound"), TEXT("Procedural sound design graph."), true},
			};
		}
		if (R == TEXT("cinematic"))
		{
			return {
				{TEXT("LevelSequencer"),    TEXT("Sequencer assets and Level Sequence editing."), true},
				{TEXT("MovieRenderQueue"),  TEXT("Production-quality offline render queue."),     true},
			};
		}

		return {};
	}

	static bool RunRecommendForRole(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString Role;
		if (!Arguments->TryGetStringField(TEXT("agent_role"), Role) || Role.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("agent_role"));
			OutError = TEXT("Missing agent_role.");
			return false;
		}

		const FString RoleLower = Role.ToLower();
		const bool bKnownRole =
			RoleLower == TEXT("foliage_author") ||
			RoleLower == TEXT("animation_author") ||
			RoleLower == TEXT("material_author") ||
			RoleLower == TEXT("vfx_author") ||
			RoleLower == TEXT("audio_author") ||
			RoleLower == TEXT("cinematic");

		const TArray<FRoleRecommendation> Rec = GetRecommendations(Role);

		TArray<TSharedPtr<FJsonValue>> RecJson;
		for (const FRoleRecommendation& R : Rec)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("plugin_name"), R.PluginName);
			Obj->SetStringField(TEXT("reason"),      R.Reason);
			Obj->SetBoolField  (TEXT("must_have"),   R.bMustHave);

			// Live status: is the plugin installed/enabled in this project?
			TSharedPtr<IPlugin> Plug = IPluginManager::Get().FindPlugin(R.PluginName);
			if (Plug.IsValid())
			{
				Obj->SetBoolField  (TEXT("installed"), true);
				Obj->SetBoolField  (TEXT("enabled"),   Plug->IsEnabled());
				Obj->SetStringField(TEXT("version"),   Plug->GetDescriptor().VersionName);
			}
			else
			{
				Obj->SetBoolField(TEXT("installed"), false);
				Obj->SetBoolField(TEXT("enabled"),   false);
			}
			RecJson.Add(MakeShared<FJsonValueObject>(Obj));
		}

		OutStructured->SetArrayField (TEXT("recommended"), RecJson);
		OutStructured->SetStringField(TEXT("agent_role"),  Role);
		OutStructured->SetNumberField(TEXT("count"),       Rec.Num());
		OutStructured->SetBoolField  (TEXT("known_role"),  bKnownRole);
		OutStructured->SetStringField(TEXT("status"),
			bKnownRole ? (Rec.IsEmpty() ? TEXT("no_recommendations") : TEXT("ok")) : TEXT("unknown_role"));

		if (!bKnownRole)
		{
			OutError = FString::Printf(TEXT("Unknown agent_role '%s'."), *Role);
			return false;
		}

		if (Rec.IsEmpty())
		{
			OutSummary = FString::Printf(TEXT("No recommendations for role '%s' (matrix returns default empty)."), *Role);
		}
		else
		{
			OutSummary = FString::Printf(TEXT("Role '%s' → %d recommended plugins."), *Role, Rec.Num());
		}
		return true;
	}

	static TArray<FString> WorldForgePluginFamilyNames()
	{
		return {
			TEXT("SOMOLWorldCore"),
			TEXT("SOMOLAuthoritySync"),
			TEXT("SOMOLRuntimeTerrain"),
			TEXT("SOMOLRuntimeContentBridge"),
			TEXT("SOMOLRuntimeHydrology"),
			TEXT("SOMOLEcologyCore"),
			TEXT("SOMOLRuntimeVegetation"),
			TEXT("SOMOLRuntimeWeather"),
			TEXT("SOMOLRuntimeNavigationClient"),
			TEXT("SOMOLArchitectureCore"),
			TEXT("SOMOLRuntimeSettlement"),
			TEXT("SOMOLEntityRuntime"),
			TEXT("SOMOLRuntimeInteraction"),
			TEXT("SOMOLRuntimeFauna")
		};
	}

	static TArray<FString> GetStringArrayArg(const TSharedRef<FJsonObject>& Arguments, const TCHAR* FieldName)
	{
		TArray<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* Raw = nullptr;
		if (!Arguments->TryGetArrayField(FieldName, Raw) || !Raw)
		{
			return Values;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Raw)
		{
			if (Value.IsValid())
			{
				const FString Text = Value->AsString();
				if (!Text.IsEmpty())
				{
					Values.Add(Text);
				}
			}
		}
		return Values;
	}

	static TSharedRef<FJsonObject> WorldForgePluginRow(const FString& Name, bool bRequired)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetBoolField(TEXT("required"), bRequired);
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
		Obj->SetBoolField(TEXT("found"), Plugin.IsValid());
		Obj->SetBoolField(TEXT("enabled"), Plugin.IsValid() && Plugin->IsEnabled());
		Obj->SetStringField(TEXT("status"), !Plugin.IsValid()
			? (bRequired ? TEXT("missing_required_plugin") : TEXT("missing_optional_plugin"))
			: Plugin->IsEnabled()
				? TEXT("ready")
				: (bRequired ? TEXT("disabled_required_plugin") : TEXT("disabled_optional_plugin")));
		if (Plugin.IsValid())
		{
			const FPluginDescriptor& Desc = Plugin->GetDescriptor();
			Obj->SetStringField(TEXT("friendly_name"), Desc.FriendlyName);
			Obj->SetStringField(TEXT("version_name"), Desc.VersionName);
			Obj->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
			Obj->SetStringField(TEXT("loaded_from"), CategoryForPlugin(Plugin.ToSharedRef()));
		}
		return Obj;
	}

	static bool RunWorldForgePluginFamilyScan(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& /*OutError*/)
	{
		TArray<FString> Required = GetStringArrayArg(Arguments, TEXT("required_plugins"));
		TSet<FString> RequiredSet;
		for (const FString& Name : Required)
		{
			RequiredSet.Add(Name.ToLower());
		}
		if (RequiredSet.Num() == 0)
		{
			RequiredSet.Add(TEXT("somolworldcore"));
		}

		int32 FoundCount = 0;
		int32 EnabledCount = 0;
		int32 MissingRequiredCount = 0;
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FString& Name : WorldForgePluginFamilyNames())
		{
			const bool bRequired = RequiredSet.Contains(Name.ToLower());
			TSharedRef<FJsonObject> Row = WorldForgePluginRow(Name, bRequired);
			const bool bFound = Row->GetBoolField(TEXT("found"));
			const bool bEnabled = Row->GetBoolField(TEXT("enabled"));
			if (bFound)
			{
				FoundCount++;
			}
			if (bEnabled)
			{
				EnabledCount++;
			}
			if (bRequired && !bEnabled)
			{
				MissingRequiredCount++;
			}
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}

		OutStructured->SetStringField(TEXT("schema"), TEXT("somolmcp.worldforge_plugin_family_scan:v1"));
		OutStructured->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		OutStructured->SetArrayField(TEXT("plugins"), Rows);
		OutStructured->SetNumberField(TEXT("family_count"), Rows.Num());
		OutStructured->SetNumberField(TEXT("found_count"), FoundCount);
		OutStructured->SetNumberField(TEXT("enabled_count"), EnabledCount);
		OutStructured->SetNumberField(TEXT("missing_required_count"), MissingRequiredCount);
		OutStructured->SetBoolField(TEXT("ok"), MissingRequiredCount == 0);
		OutStructured->SetBoolField(TEXT("fail_closed"), MissingRequiredCount != 0);
		OutStructured->SetStringField(TEXT("status"), MissingRequiredCount == 0 ? TEXT("ready") : TEXT("blocked_missing_required_plugins"));
		OutStructured->SetStringField(TEXT("receipt"), TEXT("worldforge_plugin_family_scan_receipt"));
		OutSummary = FString::Printf(
			TEXT("worldforge_plugin_family_scan: %d/%d found, %d enabled, %d required missing/disabled."),
			FoundCount,
			Rows.Num(),
			EnabledCount,
			MissingRequiredCount);
		return true;
	}

} // namespace PluginDiscoveryImpl

// ============================================================================
// Registration
// ============================================================================
void RegisterPluginDiscoveryTools(FSololmcpToolRegistry& Registry)
{
	using FSB = FSololmcpSchemaBuilder;

#if SOMOLMCP_WITH_WORLDFORGE
	// WorldForge semi-isolated plugin family gate.
	Registry.Register({
		TEXT("worldforge_plugin_family_scan"),
		TEXT("Scan the SOMOL WorldForge plugin family in the live UE editor and return enabled/missing/required status for semi-isolated large-world activation."),
		FSB::Object(
			{
				{TEXT("required_plugins"), FSB::Array(FSB::String(TEXT("WorldForge plugin names required by the current mission.")))}
			},
			/*required*/ {}),
		&PluginDiscoveryImpl::RunWorldForgePluginFamilyScan,
		nullptr,
		/*CacheTtlSeconds*/ 10
	});
#endif

	// 1) plugin_list_all
	Registry.Register({
		TEXT("plugin_list_all"),
		TEXT("List every plugin discovered by IPluginManager. Optional category_filter "
		     "narrows to engine | project | marketplace. Returns count + plugins[]."),
		FSB::Object(
			{
				{TEXT("category_filter"), FSB::String(
					TEXT("Optional: 'engine', 'project', or 'marketplace'."),
					{TEXT("engine"), TEXT("project"), TEXT("marketplace")})}
			},
			/*required*/ {}),
		&PluginDiscoveryImpl::RunListAll,
		nullptr,
		/*CacheTtlSeconds*/ 30
	});

	// 2) plugin_inspect
	Registry.Register({
		TEXT("plugin_inspect"),
		TEXT("Detailed descriptor for a single plugin: modules, dependencies, "
		     "required_engine_version, content_only, base_dir."),
		FSB::Object(
			{
				{TEXT("plugin_name"), FSB::String(TEXT("Plugin name (matches IPlugin::GetName)."))}
			},
			{TEXT("plugin_name")}),
		&PluginDiscoveryImpl::RunInspect,
		nullptr,
		/*CacheTtlSeconds*/ 30
	});

	// 3) plugin_check_compatibility
	Registry.Register({
		TEXT("plugin_check_compatibility"),
		TEXT("Compare plugin's required engine version to current ENGINE_VERSION_STRING and "
		     "scan for missing/disabled required dependencies. Returns {compatible, issues[]}."),
		FSB::Object(
			{
				{TEXT("plugin_name"), FSB::String(TEXT("Plugin name to check."))}
			},
			{TEXT("plugin_name")}),
		&PluginDiscoveryImpl::RunCheckCompatibility,
		nullptr,
		/*CacheTtlSeconds*/ 0
	});

	// 4) plugin_recommend_for_role
	Registry.Register({
		TEXT("plugin_recommend_for_role"),
		TEXT("Hardcoded role-to-plugin matrix. Roles: foliage_author, animation_author, "
		     "material_author, vfx_author, audio_author, cinematic. Each entry is annotated "
		     "with installed/enabled/version live status."),
		FSB::Object(
			{
				{TEXT("agent_role"), FSB::String(
					TEXT("Agent role token."),
					{TEXT("foliage_author"), TEXT("animation_author"), TEXT("material_author"),
					 TEXT("vfx_author"),     TEXT("audio_author"),     TEXT("cinematic")})}
			},
			{TEXT("agent_role")}),
		&PluginDiscoveryImpl::RunRecommendForRole,
		nullptr,
		/*CacheTtlSeconds*/ 60
	});
}

} // namespace UE::SOMOLMCP
