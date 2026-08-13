// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpPcgEnhancementTools.cpp — SOMOLMCP v3.7
// AI Toolkit PCG Enhancement — accuracy, effectiveness, efficiency pack.
//
// P0 tools delivered:
//   A8 pcg_node_catalog        — enumerate all UPCGSettings subclasses + pin signatures
//   A1 pcg_graph_validate      — pre-execution graph validation (pin types, dangling, loops)
//   A3 pcg_graph_explain       — human-readable semantic description of a graph
//   A9 pcg_dry_run             — Scaffold for count-only generation (body is stage 1 heuristic)
//
// V2 tools added (v3.7 V2):
//   A4 pcg_partition_preview   — tile grid + per-tile estimate, no spawn
//   A5 pcg_generate_async      — fire-and-forget via FSololmcpJobService
//   A5 pcg_job_poll            — snapshot or wait-up-to-5000ms on a job
//   A6 pcg_biome_overlay_apply — V2 writes via FInstancedPropertyBag, probe fallback
//   A7 pcg_graph_diff          — structural + property delta between two graphs
//   A12 pcg_attribute_inspect  — post-generate ISM/HISM attribute + mesh surface probe
//   pcg_character_montage_decorate — post-PCG montage assignment for scattered actors
//   pcg_resolve_graph_parameters — dump every parameter on every graph instance of an actor
//   pcg_component_info         — one-shot UPCGComponent state dump
//
// Design goals (for 5000km x 5000km MMORPG world):
//   - Catch graph errors BEFORE an 8-minute generate pass.
//   - Give the AI a searchable node type dictionary so it stops guessing names.
//   - Produce machine-readable issue lists with actionable hints.

#include "Tools/SololmcpToolRegistry.h"
#include "Protocol/SololmcpJobService.h" // A5 — async wrapper delegates to jobs framework
#include "SololmcpSchemaBuilder.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpErrorHelpers.h"
#include "Tools/SololmcpPcgExecutionSafety.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGEdge.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#if __has_include("Subsystems/PCGSubsystem.h")
#include "Subsystems/PCGSubsystem.h"
#else
#include "PCGSubsystem.h"
#endif
#include "PCGCommon.h"
#include "PCGComponent.h" // A6 — introspect UPCGComponent on actors

#include "GameFramework/Actor.h" // A6 — AActor / GetComponents<UPCGComponent>()
#include "EngineUtils.h"          // Montage decorate — TActorIterator

// A6 V2 — user parameter writes. FInstancedPropertyBag was added in 5.3 and is
// the current UE/PCG mechanism for per-graph-instance parameter overrides.
#define SOMOLMCP_COMPAT_NEED_STRUCTUTILS
#include "SololmcpEngineCompat.h"

// A12 — spawned ISM/HISM enumeration for post-generate attribute inspection.
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"

// Character montage decorate — skeletal mesh actor + AnimInstance Montage_Play.
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

#include "UObject/UObjectIterator.h"
#include "UObject/UObjectHash.h" // Audit round 7: ForEachObjectWithOuter for unwrapping UPackage in LoadPCGGraph
#include "UObject/Package.h"     // Audit round 7: UPackage for snapshot path normalization
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Crc.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "SololmcpSharedLocks.h" // v3.10.x worker-safety: BudgetStore mutex

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPPcgEnh, Log, All);

namespace UE::SOMOLMCP
{
namespace
{
	// ─── Shared helpers ──────────────────────────────────────────────────────

	static UPCGGraph* LoadPCGGraph(FSololmcpEditorServices& Services, const FString& AssetPath, FString& OutError)
	{
		UObject* Asset = Services.LoadAsset(AssetPath, OutError);
		// Audit round 7: pcg_graph_restore was failing for snapshot paths produced by
		// MakeSnapshotPath (e.g. "/Game/PCG/__Snapshots/PG_PineForest_E2E__lc_baseline" — package
		// path with no .AssetName suffix). LoadAsset's FindObject fast-path could resolve that to
		// the UPackage itself, which then fails the UPCGGraph cast and reports "Asset is not a
		// UPCGGraph or UPCGGraphInterface with a graph" even though the graph is inside the package.
		// Robustness fixes:
		//   1. If we got nothing, retry with the canonical "<path>.<leaf>" form (Unreal convention).
		//   2. If we got a UPackage, walk it and pick the first inner UPCGGraph / UPCGGraphInterface.
		if (!Asset)
		{
			int32 Slash = INDEX_NONE;
			if (AssetPath.FindLastChar(TEXT('/'), Slash) && Slash + 1 < AssetPath.Len())
			{
				FString Leaf = AssetPath.Mid(Slash + 1);
				int32 Dot = INDEX_NONE;
				if (!Leaf.FindChar(TEXT('.'), Dot))
				{
					FString Retry;
					Retry.Reserve(AssetPath.Len() + 1 + Leaf.Len());
					Retry.Append(AssetPath);
					Retry.AppendChar(TEXT('.'));
					Retry.Append(Leaf);
					FString RetryErr;
					Asset = Services.LoadAsset(Retry, RetryErr);
				}
			}
		}
		if (!Asset) return nullptr;
		if (UPackage* Pkg = Cast<UPackage>(Asset))
		{
			UPCGGraph* InnerGraph = nullptr;
			UPCGGraphInterface* InnerIface = nullptr;
			ForEachObjectWithOuter(Pkg, [&](UObject* Inner)
			{
				if (!InnerGraph) InnerGraph = Cast<UPCGGraph>(Inner);
				if (!InnerIface) InnerIface = Cast<UPCGGraphInterface>(Inner);
			}, /*bIncludeNestedObjects=*/false);
			if (InnerGraph) return InnerGraph;
			if (InnerIface)
			{
				if (UPCGGraph* G = InnerIface->GetMutablePCGGraph()) return G;
			}
			OutError = FString::Printf(TEXT("Asset path resolved to UPackage with no UPCGGraph inside: %s"), *AssetPath);
			return nullptr;
		}
		// Accept both direct UPCGGraph and UPCGGraphInterface (instance wrapper).
		if (UPCGGraph* Graph = Cast<UPCGGraph>(Asset)) return Graph;
		if (UPCGGraphInterface* GI = Cast<UPCGGraphInterface>(Asset))
		{
			if (UPCGGraph* G = GI->GetMutablePCGGraph()) return G;
		}
		OutError = TEXT("Asset is not a UPCGGraph or UPCGGraphInterface with a graph.");
		return nullptr;
	}

	static FString PinTypeToString(EPCGDataType Type)
	{
		// Friendly names aligned with UE5.7 PCG enum.
		switch (Type)
		{
		case EPCGDataType::Point:         return TEXT("Point");
		case EPCGDataType::Spline:        return TEXT("Spline");
		case EPCGDataType::LandscapeSpline:return TEXT("LandscapeSpline");
		case EPCGDataType::Landscape:     return TEXT("Landscape");
		case EPCGDataType::Texture:       return TEXT("Texture");
		case EPCGDataType::RenderTarget:  return TEXT("RenderTarget");
		case EPCGDataType::BaseTexture:   return TEXT("BaseTexture");
		case EPCGDataType::Surface:       return TEXT("Surface");
		case EPCGDataType::Volume:        return TEXT("Volume");
		case EPCGDataType::Primitive:     return TEXT("Primitive");
		case EPCGDataType::Spatial:       return TEXT("Spatial");
		case EPCGDataType::Param:         return TEXT("Param");
		case EPCGDataType::Settings:      return TEXT("Settings");
		case EPCGDataType::Other:         return TEXT("Other");
		case EPCGDataType::Any:           return TEXT("Any");
		default: break;
		}
		return FString::Printf(TEXT("Type_%u"), static_cast<uint32>(Type));
	}

	#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 7
	static FString PinTypeToString(const FPCGDataTypeIdentifier& Type)
	{
		const FString TypeString = Type.ToString();
		if (!TypeString.IsEmpty())
		{
			return TypeString;
		}
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		return PinTypeToString(static_cast<EPCGDataType>(Type));
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
	}
	#endif

	static FString CurrentPinTypeToString(const UPCGPin* Pin)
	{
		if (!Pin)
		{
			return TEXT("unknown");
		}
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 6
		return PinTypeToString(Pin->GetCurrentTypes());
	#else
		return PinTypeToString(Pin->GetCurrentTypesID());
	#endif
	}

	// 5.3 ships PCG under Engine/Plugins/Experimental and predates the pin status/usage
	// model: FPCGPinProperties there carries only bAdvancedPin, and there is no
	// IsInputPinRequiredByExecution. 5.4 introduced EPCGPinStatus/EPCGPinUsage and
	// renamed UPCGPin::AllowMultipleConnections to AllowsMultipleConnections.
	//
	// These accessors keep the 30-odd call sites below identical across versions. Where
	// 5.3 genuinely cannot answer (required, usage, override-or-user-param) they report
	// the engine's own default rather than inventing a value; where it can (advanced
	// pins, multiple connections, authored titles) they read the real 5.3 field.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 4
	enum class EPCGPinStatus : uint8 { Normal, Required, Advanced, OverrideOrUserParam };
	enum class EPCGPinUsage : uint8 { Normal, Loop, Feedback, DependencyOnly };

	static EPCGPinStatus SomolPinStatus(const FPCGPinProperties& P) { return P.bAdvancedPin ? EPCGPinStatus::Advanced : EPCGPinStatus::Normal; }
	static EPCGPinUsage SomolPinUsage(const FPCGPinProperties&) { return EPCGPinUsage::Normal; }
	static bool SomolIsRequiredPin(const FPCGPinProperties&) { return false; }
	static bool SomolIsAdvancedPin(const FPCGPinProperties& P) { return P.bAdvancedPin; }
	static bool SomolIsOverrideOrUserParamPin(const FPCGPinProperties&) { return false; }
	static bool SomolPropsAllowMultiConn(const FPCGPinProperties& P) { return P.bAllowMultipleData && P.bAllowMultipleConnections; }
	static bool SomolPinAllowMultiConn(const UPCGPin* Pin) { return Pin && Pin->AllowMultipleConnections(); }
	static bool SomolHasAuthoredTitle(const UPCGNode* N) { return N && N->NodeTitle != NAME_None; }
	static FString SomolAuthoredTitle(const UPCGNode* N) { return (N && N->NodeTitle != NAME_None) ? N->NodeTitle.ToString() : FString(); }
	static bool SomolInputPinRequiredByExecution(const UPCGNode*, const UPCGPin*) { return false; }
#else
	static EPCGPinStatus SomolPinStatus(const FPCGPinProperties& P) { return P.PinStatus; }
	static EPCGPinUsage SomolPinUsage(const FPCGPinProperties& P) { return P.Usage; }
	static bool SomolIsRequiredPin(const FPCGPinProperties& P) { return P.IsRequiredPin(); }
	static bool SomolIsAdvancedPin(const FPCGPinProperties& P) { return P.IsAdvancedPin(); }
	static bool SomolIsOverrideOrUserParamPin(const FPCGPinProperties& P)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		return P.IsOverrideOrUserParamPin();
#else
		// The OverrideOrUserParam status is 5.5+; 5.4 has only Normal/Required/Advanced.
		(void)P; return false;
#endif
	}
	static bool SomolPropsAllowMultiConn(const FPCGPinProperties& P) { return P.AllowsMultipleConnections(); }
	static bool SomolPinAllowMultiConn(const UPCGPin* Pin) { return Pin && Pin->AllowsMultipleConnections(); }
	static bool SomolHasAuthoredTitle(const UPCGNode* N) { return N && N->HasAuthoredTitle(); }
	static FString SomolAuthoredTitle(const UPCGNode* N)
	{
		if (!N || !N->HasAuthoredTitle()) { return FString(); }
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 5
		return N->GetAuthoredTitleLine().ToString();
	#else
		return N->GetAuthoredTitleName().ToString();
	#endif
	}
	static bool SomolInputPinRequiredByExecution(const UPCGNode* N, const UPCGPin* Pin) { return N && N->IsInputPinRequiredByExecution(Pin); }
#endif

	// UPCGSettings::InputPinProperties/OutputPinProperties are public from 5.4 but
	// protected on 5.3. AllInput/AllOutputPinProperties are public on both; on 5.3 they
	// additionally include the overridable-param pins, which is the closest this engine
	// version can get rather than reporting nothing.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
	static TArray<FPCGPinProperties> SomolInputPins(const UPCGSettings* S) { return S ? S->InputPinProperties() : TArray<FPCGPinProperties>(); }
	static TArray<FPCGPinProperties> SomolOutputPins(const UPCGSettings* S) { return S ? S->OutputPinProperties() : TArray<FPCGPinProperties>(); }
#else
	// InputPinProperties/OutputPinProperties only became public in 5.5; on 5.3 and 5.4
	// the public AllInput/AllOutputPinProperties are used instead.
	static TArray<FPCGPinProperties> SomolInputPins(const UPCGSettings* S) { return S ? S->AllInputPinProperties() : TArray<FPCGPinProperties>(); }
	static TArray<FPCGPinProperties> SomolOutputPins(const UPCGSettings* S) { return S ? S->AllOutputPinProperties() : TArray<FPCGPinProperties>(); }
#endif

	static FString PinStatusToString(EPCGPinStatus Status)
	{
		switch (Status)
		{
		case EPCGPinStatus::Normal: return TEXT("normal");
		case EPCGPinStatus::Required: return TEXT("required");
		case EPCGPinStatus::Advanced: return TEXT("advanced");
	// 5.3 uses the locally declared enum above (which has this member) and 5.5+ has it in
	// the engine enum. Only 5.4 sits between the two without it.
#if ENGINE_MAJOR_VERSION != 5 || ENGINE_MINOR_VERSION < 4 || ENGINE_MINOR_VERSION >= 5
		case EPCGPinStatus::OverrideOrUserParam: return TEXT("override_or_user_param");
#endif
		default: break;
		}
		return TEXT("unknown");
	}

	static FString PinUsageToString(EPCGPinUsage Usage)
	{
		switch (Usage)
		{
		case EPCGPinUsage::Normal: return TEXT("normal");
		case EPCGPinUsage::Loop: return TEXT("loop");
		case EPCGPinUsage::Feedback: return TEXT("feedback");
		case EPCGPinUsage::DependencyOnly: return TEXT("dependency_only");
		default: break;
		}
		return TEXT("unknown");
	}

	#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 7
	static FString CompatibilityResultToString(EPCGDataTypeCompatibilityResult Result)
	{
		switch (Result)
		{
		case EPCGDataTypeCompatibilityResult::Compatible: return TEXT("compatible");
		case EPCGDataTypeCompatibilityResult::RequireFilter: return TEXT("requires_filter");
		case EPCGDataTypeCompatibilityResult::RequireConversion: return TEXT("requires_conversion");
		case EPCGDataTypeCompatibilityResult::NotCompatible: return TEXT("not_compatible");
		case EPCGDataTypeCompatibilityResult::UnknownType: return TEXT("unknown_type");
		case EPCGDataTypeCompatibilityResult::TypeCompatibleSubtypeNotCompatible: return TEXT("subtype_not_compatible");
		default: break;
		}
		return TEXT("unknown");
	}
	#endif

	// Produce a human-oriented category name for a settings class.
	static FString CategorizeSettings(const UClass* SettingsCls)
	{
		const FString Name = SettingsCls->GetName();
		if (Name.Contains(TEXT("Landscape"))) return TEXT("landscape");
		if (Name.Contains(TEXT("Spline")))    return TEXT("spline");
		if (Name.Contains(TEXT("Sampler")))   return TEXT("sampling");
		if (Name.Contains(TEXT("Filter")))    return TEXT("filter");
		if (Name.Contains(TEXT("Density")))   return TEXT("density");
		if (Name.Contains(TEXT("Transform"))) return TEXT("transform");
		if (Name.Contains(TEXT("Spawn")) || Name.Contains(TEXT("Mesh"))) return TEXT("spawn");
		if (Name.Contains(TEXT("Attribute"))) return TEXT("attribute");
		if (Name.Contains(TEXT("Math")) || Name.Contains(TEXT("Op"))) return TEXT("math");
		if (Name.Contains(TEXT("Debug")))     return TEXT("debug");
		if (Name.Contains(TEXT("Subgraph"))||Name.Contains(TEXT("Graph"))) return TEXT("graph");
		return TEXT("other");
	}

	static FString StableIssueId(
		const FString& AssetPath,
		const FString& Code,
		const FString& NodeName,
		const FString& PinLabel,
		const FString& Message)
	{
		const FString StableText = FString::Printf(TEXT("%s|%s|%s|%s|%s"),
			*AssetPath, *Code, *NodeName, *PinLabel, *Message);
		return FString::Printf(TEXT("pcg_%s_%08x"), *Code, FCrc::StrCrc32(*StableText));
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static FString PcgProbeEngineVersionString()
	{
		return FString::Printf(TEXT("%d.%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION, ENGINE_PATCH_VERSION);
	}

	static void AttachPcg58ProbeBaseFields(TSharedRef<FJsonObject>& OutStructured, const FString& ToolName)
	{
		OutStructured->SetStringField(TEXT("tool_name"), ToolName);
		OutStructured->SetStringField(TEXT("engine_version"), PcgProbeEngineVersionString());
		OutStructured->SetStringField(TEXT("required_engine_version"), TEXT("5.8+"));
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetBoolField(TEXT("dry_run"), true);
		OutStructured->SetBoolField(TEXT("mutation_attempted"), false);
		OutStructured->SetBoolField(TEXT("generation_dispatched"), false);
	}

	static bool ReturnPcg58Unavailable(
		const FString& ToolName,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary)
	{
		AttachPcg58ProbeBaseFields(OutStructured, ToolName);
		OutStructured->SetBoolField(TEXT("available"), false);
		OutStructured->SetStringField(TEXT("status"), TEXT("version_unavailable"));
		OutStructured->SetStringField(TEXT("unsupported_reason"), TEXT("This probe is UE 5.8+ only; the current build returns a guarded no-op response."));
		OutSummary = FString::Printf(TEXT("%s: version_unavailable on UE %s"), *ToolName, *PcgProbeEngineVersionString());
		return true;
	}

	static bool IsModuleKnownOrLoaded(const FName ModuleName)
	{
		FModuleManager& ModuleManager = FModuleManager::Get();
		const FString ModuleNameString = ModuleName.ToString();
		return ModuleManager.IsModuleLoaded(ModuleName) || ModuleManager.ModuleExists(*ModuleNameString);
	}

	static void AttachModuleAvailability(TSharedRef<FJsonObject>& OutStructured)
	{
		const FName PcgModuleName(TEXT("PCG"));
		const FName PcgComputeModuleName(TEXT("PCGCompute"));
		FModuleManager& ModuleManager = FModuleManager::Get();
		OutStructured->SetBoolField(TEXT("pcg_module_loaded"), ModuleManager.IsModuleLoaded(PcgModuleName));
		OutStructured->SetBoolField(TEXT("pcg_module_available"), IsModuleKnownOrLoaded(PcgModuleName));
		OutStructured->SetBoolField(TEXT("pcgcompute_module_loaded"), ModuleManager.IsModuleLoaded(PcgComputeModuleName));
		OutStructured->SetBoolField(TEXT("pcgcompute_module_available"), IsModuleKnownOrLoaded(PcgComputeModuleName));
	}

	static FString PcgSettingsTypeToString(EPCGSettingsType Type)
	{
		if (const UEnum* Enum = StaticEnum<EPCGSettingsType>())
		{
			const FString Name = Enum->GetNameStringByValue(static_cast<int64>(Type));
			if (!Name.IsEmpty())
			{
				return Name;
			}
		}
		return FString::Printf(TEXT("EPCGSettingsType_%d"), static_cast<int32>(Type));
	}

	static void AttachConsoleVariableSnapshot(TSharedRef<FJsonObject>& OutStructured, const TCHAR* FieldName, const TCHAR* CVarName)
	{
		TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		Snapshot->SetStringField(TEXT("name"), CVarName);
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(CVarName))
		{
			Snapshot->SetBoolField(TEXT("available"), true);
			Snapshot->SetStringField(TEXT("value_string"), CVar->GetString());
			Snapshot->SetNumberField(TEXT("value_float"), CVar->GetFloat());
			Snapshot->SetNumberField(TEXT("value_int"), CVar->GetInt());
		}
		else
		{
			Snapshot->SetBoolField(TEXT("available"), false);
		}
		OutStructured->SetObjectField(FieldName, Snapshot);
	}

	static FString NodeDisplayName(const UPCGNode* Node)
	{
		if (!Node)
		{
			return TEXT("");
		}
		if (SomolHasAuthoredTitle(Node))
		{
			const FString Authored = SomolAuthoredTitle(Node);
			if (!Authored.IsEmpty())
			{
				return Authored;
			}
		}
		return Node->GetName();
	}

	static bool SplitPinPath(const FString& InPath, FString& OutNode, FString& OutPin)
	{
		FString Left;
		FString Right;
		if (InPath.Split(TEXT("::"), &Left, &Right) || InPath.Split(TEXT("|"), &Left, &Right))
		{
			OutNode = Left.TrimStartAndEnd();
			OutPin = Right.TrimStartAndEnd();
			return !OutNode.IsEmpty() && !OutPin.IsEmpty();
		}
		return false;
	}

	static bool PinLabelMatches(const UPCGPin* Pin, const FString& Label)
	{
		return Pin
			&& (Label.IsEmpty()
				|| Pin->Properties.Label.ToString().Equals(Label, ESearchCase::IgnoreCase)
				|| Pin->GetName().Equals(Label, ESearchCase::IgnoreCase));
	}

	static const UPCGNode* FindNodeByIdentifier(const UPCGGraph* Graph, const FString& Identifier, FString* OutError = nullptr)
	{
		if (!Graph)
		{
			if (OutError) *OutError = TEXT("Graph is null.");
			return nullptr;
		}
		const FString Needle = Identifier.TrimStartAndEnd();
		if (Needle.IsEmpty())
		{
			if (OutError) *OutError = TEXT("Node identifier is empty.");
			return nullptr;
		}

		TArray<const UPCGNode*> PartialMatches;
		for (const UPCGNode* Node : Graph->GetNodes())
		{
			if (!Node) continue;
			const FString RawName = Node->GetName();
			const FString Display = NodeDisplayName(Node);
			const FString Authored = SomolAuthoredTitle(Node);
			if (RawName.Equals(Needle, ESearchCase::IgnoreCase)
				|| Display.Equals(Needle, ESearchCase::IgnoreCase)
				|| (!Authored.IsEmpty() && Authored.Equals(Needle, ESearchCase::IgnoreCase)))
			{
				return Node;
			}
			if (RawName.Contains(Needle, ESearchCase::IgnoreCase)
				|| Display.Contains(Needle, ESearchCase::IgnoreCase)
				|| (!Authored.IsEmpty() && Authored.Contains(Needle, ESearchCase::IgnoreCase)))
			{
				PartialMatches.Add(Node);
			}
		}

		if (PartialMatches.Num() == 1)
		{
			return PartialMatches[0];
		}
		if (OutError)
		{
			*OutError = PartialMatches.Num() > 1
				? FString::Printf(TEXT("Node identifier '%s' is ambiguous (%d partial matches)."), *Needle, PartialMatches.Num())
				: FString::Printf(TEXT("Node identifier '%s' was not found."), *Needle);
		}
		return nullptr;
	}

	static UPCGNode* FindNodeByIdentifier(UPCGGraph* Graph, const FString& Identifier, FString* OutError = nullptr)
	{
		return const_cast<UPCGNode*>(FindNodeByIdentifier(const_cast<const UPCGGraph*>(Graph), Identifier, OutError));
	}

	static const UPCGPin* FindPinByLabel(const UPCGNode* Node, const FString& Label, bool bOutputPin, FString* OutError = nullptr)
	{
		if (!Node)
		{
			if (OutError) *OutError = TEXT("Node is null.");
			return nullptr;
		}

		const TArray<TObjectPtr<UPCGPin>>& Pins = bOutputPin ? Node->GetOutputPins() : Node->GetInputPins();
		for (const TObjectPtr<UPCGPin>& Pin : Pins)
		{
			if (PinLabelMatches(Pin.Get(), Label))
			{
				return Pin.Get();
			}
		}

		if (OutError)
		{
			TArray<FString> Labels;
			for (const TObjectPtr<UPCGPin>& Pin : Pins)
			{
				if (Pin)
				{
					Labels.Add(Pin->Properties.Label.ToString());
				}
			}
			*OutError = FString::Printf(TEXT("%s pin '%s' was not found on node '%s'. Available: %s"),
				bOutputPin ? TEXT("Output") : TEXT("Input"),
				*Label,
				*NodeDisplayName(Node),
				*FString::Join(Labels, TEXT(", ")));
		}
		return nullptr;
	}

	static UPCGPin* FindPinByLabel(UPCGNode* Node, const FString& Label, bool bOutputPin, FString* OutError = nullptr)
	{
		return const_cast<UPCGPin*>(FindPinByLabel(const_cast<const UPCGNode*>(Node), Label, bOutputPin, OutError));
	}

	static bool TryResolvePinReference(
		UPCGGraph* Graph,
		const FString& NodeField,
		const FString& PinField,
		const FString& PinPathField,
		bool bOutputPin,
		const TSharedRef<FJsonObject>& Arguments,
		UPCGNode*& OutNode,
		UPCGPin*& OutPin,
		FString& OutError)
	{
		FString NodeName;
		FString PinLabel;
		FString PinPath;
		Arguments->TryGetStringField(PinPathField, PinPath);
		if (!PinPath.IsEmpty())
		{
			if (!SplitPinPath(PinPath, NodeName, PinLabel))
			{
				OutError = FString::Printf(TEXT("Invalid %s format. Expected 'Node::Pin'."), *PinPathField);
				return false;
			}
		}
		else
		{
			Arguments->TryGetStringField(NodeField, NodeName);
			Arguments->TryGetStringField(PinField, PinLabel);
		}

		if (NodeName.TrimStartAndEnd().IsEmpty() || PinLabel.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("Missing required pin reference: %s/%s or %s."), *NodeField, *PinField, *PinPathField);
			return false;
		}

		FString NodeError;
		OutNode = FindNodeByIdentifier(Graph, NodeName, &NodeError);
		if (!OutNode)
		{
			OutError = NodeError;
			return false;
		}

		FString PinError;
		OutPin = FindPinByLabel(OutNode, PinLabel, bOutputPin, &PinError);
		if (!OutPin)
		{
			OutError = PinError;
			return false;
		}
		return true;
	}

	static TSharedRef<FJsonObject> MakePinJson(const UPCGPin* Pin)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		if (!Pin)
		{
			O->SetStringField(TEXT("label"), TEXT(""));
			O->SetBoolField(TEXT("exists"), false);
			return O;
		}
		O->SetBoolField(TEXT("exists"), true);
		O->SetStringField(TEXT("node"), Pin->Node ? NodeDisplayName(Pin->Node) : TEXT(""));
		O->SetStringField(TEXT("node_object_name"), Pin->Node ? Pin->Node->GetName() : TEXT(""));
		O->SetStringField(TEXT("label"), Pin->Properties.Label.ToString());
		O->SetStringField(TEXT("type"), PinTypeToString(Pin->Properties.AllowedTypes));
		O->SetStringField(TEXT("current_type"), CurrentPinTypeToString(Pin));
		O->SetStringField(TEXT("direction"), Pin->IsOutputPin() ? TEXT("output") : TEXT("input"));
		O->SetStringField(TEXT("status"), PinStatusToString(SomolPinStatus(Pin->Properties)));
		O->SetStringField(TEXT("usage"), PinUsageToString(SomolPinUsage(Pin->Properties)));
		O->SetBoolField(TEXT("required"), SomolIsRequiredPin(Pin->Properties));
		O->SetBoolField(TEXT("advanced"), SomolIsAdvancedPin(Pin->Properties));
		O->SetBoolField(TEXT("multi_conn"), SomolPinAllowMultiConn(Pin));
		O->SetNumberField(TEXT("edge_count"), Pin->Edges.Num());
		return O;
	}

	static bool ArePinsCompatible(const UPCGPin* SourcePin, const UPCGPin* TargetPin, TSharedRef<FJsonObject>& OutCompatibility, bool bCheckTargetCapacity = true)
	{
		OutCompatibility->SetObjectField(TEXT("source_pin"), MakePinJson(SourcePin));
		OutCompatibility->SetObjectField(TEXT("target_pin"), MakePinJson(TargetPin));

		if (!SourcePin || !TargetPin)
		{
			OutCompatibility->SetBoolField(TEXT("compatible"), false);
			OutCompatibility->SetStringField(TEXT("compatibility"), TEXT("missing_pin"));
			OutCompatibility->SetStringField(TEXT("message"), TEXT("Source or target pin was not resolved."));
			return false;
		}

		FText CompatibilityMessage;
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 5
		const bool bTypeCompatible = SourcePin->IsCompatible(TargetPin);
		const FString CompatibilityName = bTypeCompatible ? TEXT("compatible") : TEXT("not_compatible");
	#elif ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 6
		const bool bTypeCompatible = TargetPin->IsDownstreamPinTypeCompatible(SourcePin->GetCurrentTypes());
		const FString CompatibilityName = bTypeCompatible ? TEXT("compatible") : TEXT("not_compatible");
	#else
		const EPCGDataTypeCompatibilityResult Result = SourcePin->GetCompatibilityWithOtherPin(TargetPin, &CompatibilityMessage);
		const bool bTypeCompatible = PCGDataTypeCompatibilityResult::IsValid(Result);
		const FString CompatibilityName = CompatibilityResultToString(Result);
	#endif
		const bool bSourceIsOutput = SourcePin->IsOutputPin();
		const bool bTargetIsInput = !TargetPin->IsOutputPin();
		const bool bSameGraph = SourcePin->Node && TargetPin->Node && SourcePin->Node->GetGraph() == TargetPin->Node->GetGraph();
		const bool bCapacityOk = !bCheckTargetCapacity || TargetPin->Edges.IsEmpty() || SomolPinAllowMultiConn(TargetPin);
		const bool bCanConnect = bTypeCompatible && bSourceIsOutput && bTargetIsInput && bSameGraph && bCapacityOk;

		OutCompatibility->SetBoolField(TEXT("compatible"), bCanConnect);
		OutCompatibility->SetBoolField(TEXT("type_compatible"), bTypeCompatible);
		OutCompatibility->SetBoolField(TEXT("direction_ok"), bSourceIsOutput && bTargetIsInput);
		OutCompatibility->SetBoolField(TEXT("same_graph"), bSameGraph);
		OutCompatibility->SetBoolField(TEXT("target_capacity_ok"), bCapacityOk);
		OutCompatibility->SetStringField(TEXT("compatibility"), CompatibilityName);
		OutCompatibility->SetStringField(TEXT("expected"), PinTypeToString(TargetPin->Properties.AllowedTypes));
		OutCompatibility->SetStringField(TEXT("actual"), CurrentPinTypeToString(SourcePin));
		OutCompatibility->SetStringField(TEXT("message"), CompatibilityMessage.ToString());
		if (!bCanConnect)
		{
			TArray<FString> Reasons;
			if (!bSourceIsOutput || !bTargetIsInput) Reasons.Add(TEXT("Pins must be source output -> target input."));
			if (!bSameGraph) Reasons.Add(TEXT("Pins belong to different graphs."));
			if (!bTypeCompatible) Reasons.Add(TEXT("PCG data types are not compatible."));
			if (!bCapacityOk) Reasons.Add(TEXT("Target input does not allow another connection."));
			OutCompatibility->SetArrayField(TEXT("reasons"), StringArrayJson(Reasons));
		}
		return bCanConnect;
	}

	static void AddValidationIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		int32& ErrorCount,
		int32& WarningCount,
		const FString& AssetPath,
		const FString& Severity,
		const FString& Code,
		const FString& NodeName,
		const FString& PinLabel,
		const FString& Message,
		const FString& Hint,
		const FString& Strictness = TEXT("error"))
	{
		FString EffectiveSeverity = Severity;
		if (Strictness.Equals(TEXT("warn"), ESearchCase::IgnoreCase) && EffectiveSeverity == TEXT("error"))
		{
			EffectiveSeverity = TEXT("warning");
		}
		else if (Strictness.Equals(TEXT("permissive"), ESearchCase::IgnoreCase) && EffectiveSeverity != TEXT("info"))
		{
			EffectiveSeverity = TEXT("warning");
		}

		TSharedRef<FJsonObject> I = MakeShared<FJsonObject>();
		I->SetStringField(TEXT("issue_id"), StableIssueId(AssetPath, Code, NodeName, PinLabel, Message));
		I->SetStringField(TEXT("severity"), EffectiveSeverity);
		I->SetStringField(TEXT("original_severity"), Severity);
		I->SetStringField(TEXT("code"), Code);
		I->SetStringField(TEXT("node"), NodeName);
		I->SetStringField(TEXT("pin"), PinLabel);
		I->SetStringField(TEXT("message"), Message);
		I->SetStringField(TEXT("hint"), Hint);
		Issues.Add(MakeShared<FJsonValueObject>(I));
		if (EffectiveSeverity == TEXT("error")) ++ErrorCount;
		else if (EffectiveSeverity == TEXT("warning")) ++WarningCount;
	}

	static void CollectGraphCycles(const UPCGGraph* Graph, TArray<TArray<const UPCGNode*>>& OutCycles, int32 MaxCycles = 32)
	{
		if (!Graph) return;

		TMap<const UPCGNode*, TArray<const UPCGNode*>> Adj;
		for (const UPCGNode* Node : Graph->GetNodes())
		{
			if (!Node) continue;
			TArray<const UPCGNode*>& Neighbors = Adj.FindOrAdd(Node);
			for (const UPCGPin* OutPin : Node->GetOutputPins())
			{
				if (!OutPin) continue;
				for (const TObjectPtr<UPCGEdge>& Edge : OutPin->Edges)
				{
					const UPCGPin* DstPin = Edge ? Edge->OutputPin.Get() : nullptr;
					const UPCGNode* DstNode = DstPin ? DstPin->Node.Get() : nullptr;
					if (DstNode)
					{
						Neighbors.AddUnique(DstNode);
					}
				}
			}
		}

		TMap<const UPCGNode*, int32> State; // 0 unseen, 1 visiting, 2 done
		TArray<const UPCGNode*> Stack;
		TSet<FString> SeenCycleKeys;

		TFunction<void(const UPCGNode*)> Visit = [&](const UPCGNode* Node)
		{
			if (!Node || OutCycles.Num() >= MaxCycles) return;
			State.Add(Node, 1);
			Stack.Add(Node);

			const TArray<const UPCGNode*>* Neighbors = Adj.Find(Node);
			if (Neighbors)
			{
				for (const UPCGNode* Next : *Neighbors)
				{
					if (!Next) continue;
					const int32* NextState = State.Find(Next);
					if (!NextState)
					{
						Visit(Next);
					}
					else if (*NextState == 1)
					{
						int32 StartIndex = INDEX_NONE;
						for (int32 Index = 0; Index < Stack.Num(); ++Index)
						{
							if (Stack[Index] == Next)
							{
								StartIndex = Index;
								break;
							}
						}
						if (StartIndex != INDEX_NONE)
						{
							TArray<const UPCGNode*> Cycle;
							TArray<FString> Names;
							for (int32 Index = StartIndex; Index < Stack.Num(); ++Index)
							{
								Cycle.Add(Stack[Index]);
								Names.Add(NodeDisplayName(Stack[Index]));
							}
							Cycle.Add(Next);
							Names.Add(NodeDisplayName(Next));
							const FString Key = FString::Join(Names, TEXT("->"));
							if (!SeenCycleKeys.Contains(Key))
							{
								SeenCycleKeys.Add(Key);
								OutCycles.Add(Cycle);
								if (OutCycles.Num() >= MaxCycles) break;
							}
						}
					}
				}
			}

			Stack.Pop(SOMOLMCP_NO_SHRINK);
			State.Add(Node, 2);
		};

		for (const UPCGNode* Node : Graph->GetNodes())
		{
			if (!Node) continue;
			if (!State.Contains(Node))
			{
				Visit(Node);
				if (OutCycles.Num() >= MaxCycles) break;
			}
		}
	}

	static TArray<TSharedPtr<FJsonValue>> CyclesToJson(const TArray<TArray<const UPCGNode*>>& Cycles)
	{
		TArray<TSharedPtr<FJsonValue>> JsonCycles;
		for (const TArray<const UPCGNode*>& Cycle : Cycles)
		{
			TArray<FString> Names;
			for (const UPCGNode* Node : Cycle)
			{
				Names.Add(NodeDisplayName(Node));
			}
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetArrayField(TEXT("path"), StringArrayJson(Names));
			C->SetStringField(TEXT("path_string"), FString::Join(Names, TEXT(" -> ")));
			C->SetNumberField(TEXT("node_count"), FMath::Max(0, Names.Num() - 1));
			JsonCycles.Add(MakeShared<FJsonValueObject>(C));
		}
		return JsonCycles;
	}

} // namespace

// ─── Forward declarations (cross-reference between A9/A11 helpers) ─────────
static void AttachBudgetStatus(TSharedRef<FJsonObject>& OutStructured, const FString& AssetPath, double EstimatedPoints);

// ═══════════════════════════════════════════════════════════════════════════════
// A8: pcg_node_catalog — enumerate all UPCGSettings subclasses
// ═══════════════════════════════════════════════════════════════════════════════
static bool Tool_PcgNodeCatalog(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString Prefix;
	Arguments->TryGetStringField(TEXT("name_prefix"), Prefix);
	FString Category;
	Arguments->TryGetStringField(TEXT("category"), Category);
	bool bIncludePins = true;
	Arguments->TryGetBoolField(TEXT("include_pins"), bIncludePins);
	int32 Limit = 500;
	double LimitD = 0.0;
	if (Arguments->TryGetNumberField(TEXT("limit"), LimitD))
	{
		Limit = FMath::Clamp(static_cast<int32>(LimitD), 1, 2000);
	}

	TArray<UClass*> Derived;
	GetDerivedClasses(UPCGSettings::StaticClass(), Derived, /*bRecursive=*/true);

	TArray<TSharedPtr<FJsonValue>> Entries;
	int32 Skipped = 0;
	for (UClass* Cls : Derived)
	{
		if (!Cls) continue;
		if (Cls->HasAnyClassFlags(CLASS_Abstract|CLASS_Deprecated|CLASS_NewerVersionExists)) { ++Skipped; continue; }
		const FString ClassName = Cls->GetName();
		if (!Prefix.IsEmpty() && !ClassName.StartsWith(Prefix, ESearchCase::IgnoreCase)) continue;

		const FString Cat = CategorizeSettings(Cls);
		if (!Category.IsEmpty() && !Category.Equals(Cat, ESearchCase::IgnoreCase)) continue;

		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("class"), ClassName);
		E->SetStringField(TEXT("path"), Cls->GetPathName());
		E->SetStringField(TEXT("category"), Cat);

		// Pull the CDO to describe default pins.
		if (bIncludePins)
		{
			if (UPCGSettings* CDO = Cast<UPCGSettings>(Cls->GetDefaultObject(/*bCreateIfNeeded=*/true)))
			{
				TArray<TSharedPtr<FJsonValue>> InputPins, OutputPins;
				for (const FPCGPinProperties& P : SomolInputPins(CDO))
				{
					TSharedRef<FJsonObject> Po = MakeShared<FJsonObject>();
					Po->SetStringField(TEXT("label"), P.Label.ToString());
					Po->SetStringField(TEXT("type"), PinTypeToString(P.AllowedTypes));
					Po->SetStringField(TEXT("status"), PinStatusToString(SomolPinStatus(P)));
					Po->SetStringField(TEXT("usage"), PinUsageToString(SomolPinUsage(P)));
					Po->SetBoolField(TEXT("required"), SomolIsRequiredPin(P));
					Po->SetBoolField(TEXT("advanced"), SomolIsAdvancedPin(P));
					Po->SetBoolField(TEXT("multi_conn"), SomolPropsAllowMultiConn(P));
					InputPins.Add(MakeShared<FJsonValueObject>(Po));
				}
				for (const FPCGPinProperties& P : SomolOutputPins(CDO))
				{
					TSharedRef<FJsonObject> Po = MakeShared<FJsonObject>();
					Po->SetStringField(TEXT("label"), P.Label.ToString());
					Po->SetStringField(TEXT("type"), PinTypeToString(P.AllowedTypes));
					Po->SetStringField(TEXT("status"), PinStatusToString(SomolPinStatus(P)));
					Po->SetStringField(TEXT("usage"), PinUsageToString(SomolPinUsage(P)));
					Po->SetBoolField(TEXT("required"), false);
					Po->SetBoolField(TEXT("advanced"), SomolIsAdvancedPin(P));
					Po->SetBoolField(TEXT("multi_conn"), SomolPropsAllowMultiConn(P));
					OutputPins.Add(MakeShared<FJsonValueObject>(Po));
				}
				E->SetArrayField(TEXT("input_pins"), InputPins);
				E->SetArrayField(TEXT("output_pins"), OutputPins);
			}
		}

		Entries.Add(MakeShared<FJsonValueObject>(E));
		if (Entries.Num() >= Limit) break;
	}

	// Sort by class name ascending for stable output.
	Entries.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetStringField(TEXT("class")) < B->AsObject()->GetStringField(TEXT("class"));
	});

	OutStructured->SetArrayField(TEXT("nodes"), Entries);
	OutStructured->SetNumberField(TEXT("total_returned"), Entries.Num());
	OutStructured->SetNumberField(TEXT("total_abstract_skipped"), Skipped);
	OutSummary = FString::Printf(TEXT("Node catalog: %d entries (prefix='%s', category='%s')"),
		Entries.Num(), *Prefix, *Category);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Direction 4: pcg_node_catalog_lookup — searchable, pin-aware catalog lookup
// ═══════════════════════════════════════════════════════════════════════════════
static bool Tool_PcgNodeCatalogLookup(
	const FSololmcpToolExecutionContext& /*Context*/,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& /*OutError*/)
{
	FString Query;
	Arguments->TryGetStringField(TEXT("query"), Query);
	FString ClassContains;
	Arguments->TryGetStringField(TEXT("class"), ClassContains);
	if (ClassContains.IsEmpty())
	{
		Arguments->TryGetStringField(TEXT("class_contains"), ClassContains);
	}
	FString PathContains;
	Arguments->TryGetStringField(TEXT("path"), PathContains);
	if (PathContains.IsEmpty())
	{
		Arguments->TryGetStringField(TEXT("path_contains"), PathContains);
	}
	FString Category;
	Arguments->TryGetStringField(TEXT("category"), Category);
	FString PinType;
	Arguments->TryGetStringField(TEXT("pin_type"), PinType);
	FString PinLabel;
	Arguments->TryGetStringField(TEXT("pin_label"), PinLabel);
	FString Direction;
	Arguments->TryGetStringField(TEXT("direction"), Direction);
	bool bIncludePins = true;
	Arguments->TryGetBoolField(TEXT("include_pins"), bIncludePins);
	int32 Limit = 50;
	double LimitD = 0.0;
	if (Arguments->TryGetNumberField(TEXT("limit"), LimitD))
	{
		Limit = FMath::Clamp(static_cast<int32>(LimitD), 1, 500);
	}

	TArray<UClass*> Derived;
	GetDerivedClasses(UPCGSettings::StaticClass(), Derived, /*bRecursive=*/true);

	TArray<TSharedPtr<FJsonValue>> Matches;
	int32 CandidateCount = 0;
	int32 PinFilteredCount = 0;

	for (UClass* Cls : Derived)
	{
		if (!Cls || Cls->HasAnyClassFlags(CLASS_Abstract|CLASS_Deprecated|CLASS_NewerVersionExists)) continue;
		++CandidateCount;
		const FString ClassName = Cls->GetName();
		const FString ClassPath = Cls->GetPathName();
		const FString Cat = CategorizeSettings(Cls);

		TArray<FString> Reasons;
		if (!Query.IsEmpty())
		{
			const bool bQueryMatch = ClassName.Contains(Query, ESearchCase::IgnoreCase)
				|| ClassPath.Contains(Query, ESearchCase::IgnoreCase)
				|| Cat.Contains(Query, ESearchCase::IgnoreCase);
			if (!bQueryMatch) continue;
			Reasons.Add(TEXT("query"));
		}
		if (!ClassContains.IsEmpty())
		{
			if (!ClassName.Contains(ClassContains, ESearchCase::IgnoreCase)
				&& !ClassPath.Contains(ClassContains, ESearchCase::IgnoreCase))
			{
				continue;
			}
			Reasons.Add(TEXT("class"));
		}
		if (!PathContains.IsEmpty())
		{
			if (!ClassPath.Contains(PathContains, ESearchCase::IgnoreCase)) continue;
			Reasons.Add(TEXT("path"));
		}
		if (!Category.IsEmpty())
		{
			if (!Cat.Equals(Category, ESearchCase::IgnoreCase)) continue;
			Reasons.Add(TEXT("category"));
		}

		UPCGSettings* CDO = Cast<UPCGSettings>(Cls->GetDefaultObject(/*bCreateIfNeeded=*/true));
		TArray<TSharedPtr<FJsonValue>> InputPins;
		TArray<TSharedPtr<FJsonValue>> OutputPins;
		bool bPinMatched = PinType.IsEmpty() && PinLabel.IsEmpty() && Direction.IsEmpty();

		auto AppendPin = [&](const FPCGPinProperties& P, bool bOutput)
		{
			const FString TypeString = PinTypeToString(P.AllowedTypes);
			const bool bDirectionOk = Direction.IsEmpty()
				|| (bOutput && Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase))
				|| (!bOutput && Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase))
				|| Direction.Equals(TEXT("any"), ESearchCase::IgnoreCase);
			const bool bTypeOk = PinType.IsEmpty() || TypeString.Contains(PinType, ESearchCase::IgnoreCase);
			const bool bLabelOk = PinLabel.IsEmpty() || P.Label.ToString().Contains(PinLabel, ESearchCase::IgnoreCase);
			const bool bThisPinMatched = bDirectionOk && bTypeOk && bLabelOk;
			bPinMatched = bPinMatched || bThisPinMatched;

			if (!bIncludePins) return;
			TSharedRef<FJsonObject> Po = MakeShared<FJsonObject>();
			Po->SetStringField(TEXT("label"), P.Label.ToString());
			Po->SetStringField(TEXT("type"), TypeString);
			Po->SetStringField(TEXT("direction"), bOutput ? TEXT("output") : TEXT("input"));
			Po->SetStringField(TEXT("status"), PinStatusToString(SomolPinStatus(P)));
			Po->SetStringField(TEXT("usage"), PinUsageToString(SomolPinUsage(P)));
			Po->SetBoolField(TEXT("required"), !bOutput && SomolIsRequiredPin(P));
			Po->SetBoolField(TEXT("advanced"), SomolIsAdvancedPin(P));
			Po->SetBoolField(TEXT("multi_conn"), SomolPropsAllowMultiConn(P));
			Po->SetBoolField(TEXT("matched_filter"), bThisPinMatched);
			(bOutput ? OutputPins : InputPins).Add(MakeShared<FJsonValueObject>(Po));
		};

		if (CDO)
		{
			for (const FPCGPinProperties& P : SomolInputPins(CDO)) AppendPin(P, false);
			for (const FPCGPinProperties& P : SomolOutputPins(CDO)) AppendPin(P, true);
		}
		if (!bPinMatched)
		{
			++PinFilteredCount;
			continue;
		}
		if (!PinType.IsEmpty()) Reasons.Add(TEXT("pin_type"));
		if (!PinLabel.IsEmpty()) Reasons.Add(TEXT("pin_label"));
		if (!Direction.IsEmpty()) Reasons.Add(TEXT("direction"));

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class"), ClassName);
		Entry->SetStringField(TEXT("path"), ClassPath);
		Entry->SetStringField(TEXT("category"), Cat);
		Entry->SetArrayField(TEXT("matched_by"), StringArrayJson(Reasons));
		if (bIncludePins)
		{
			Entry->SetArrayField(TEXT("input_pins"), InputPins);
			Entry->SetArrayField(TEXT("output_pins"), OutputPins);
		}
		Matches.Add(MakeShared<FJsonValueObject>(Entry));
		if (Matches.Num() >= Limit) break;
	}

	Matches.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetStringField(TEXT("class")) < B->AsObject()->GetStringField(TEXT("class"));
	});

	OutStructured->SetStringField(TEXT("query"), Query);
	OutStructured->SetStringField(TEXT("class_filter"), ClassContains);
	OutStructured->SetStringField(TEXT("path_filter"), PathContains);
	OutStructured->SetStringField(TEXT("category"), Category);
	OutStructured->SetStringField(TEXT("pin_type"), PinType);
	OutStructured->SetStringField(TEXT("pin_label"), PinLabel);
	OutStructured->SetStringField(TEXT("direction"), Direction);
	OutStructured->SetNumberField(TEXT("candidate_count"), CandidateCount);
	OutStructured->SetNumberField(TEXT("pin_filtered_count"), PinFilteredCount);
	OutStructured->SetNumberField(TEXT("total_returned"), Matches.Num());
	OutStructured->SetArrayField(TEXT("matches"), Matches);
	OutSummary = FString::Printf(TEXT("PCG catalog lookup returned %d node class(es)."), Matches.Num());
	return true;
}

static TSharedRef<FJsonObject> BuildRequiredPinAuditJson(const UPCGGraph* Graph)
{
	TSharedRef<FJsonObject> Audit = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> NodesJson;
	TArray<TSharedPtr<FJsonValue>> MissingRequiredJson;
	int32 RequiredCount = 0;
	int32 OptionalCount = 0;
	int32 MissingRequiredCount = 0;
	int32 ConnectedRequiredCount = 0;

	if (!Graph)
	{
		Audit->SetArrayField(TEXT("nodes"), NodesJson);
		Audit->SetNumberField(TEXT("required_pin_count"), 0);
		Audit->SetNumberField(TEXT("optional_pin_count"), 0);
		Audit->SetNumberField(TEXT("missing_required_count"), 0);
		Audit->SetArrayField(TEXT("missing_required_pins"), MissingRequiredJson);
		return Audit;
	}

	for (const UPCGNode* Node : Graph->GetNodes())
	{
		if (!Node) continue;
		TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("node"), NodeDisplayName(Node));
		NodeJson->SetStringField(TEXT("node_object_name"), Node->GetName());
		NodeJson->SetStringField(TEXT("class"), Node->GetSettings() ? Node->GetSettings()->GetClass()->GetName() : TEXT(""));

		TArray<TSharedPtr<FJsonValue>> PinJsonArray;
		for (const UPCGPin* InPin : Node->GetInputPins())
		{
			if (!InPin) continue;
			const bool bUsed = Node->IsPinUsedByNodeExecution(InPin);
			const bool bRequired = bUsed && SomolInputPinRequiredByExecution(Node, InPin);
			const int32 EdgeCount = InPin->Edges.Num();
			if (bRequired) ++RequiredCount;
			else ++OptionalCount;
			if (bRequired && EdgeCount == 0) ++MissingRequiredCount;
			if (bRequired && EdgeCount > 0) ++ConnectedRequiredCount;

			TSharedRef<FJsonObject> PinJson = MakePinJson(InPin);
			PinJson->SetBoolField(TEXT("required_by_execution"), bRequired);
			PinJson->SetBoolField(TEXT("used_by_execution"), bUsed);
			PinJson->SetBoolField(TEXT("connected"), EdgeCount > 0);
			PinJson->SetStringField(TEXT("default_status"),
				bRequired ? TEXT("no_default_required_connection") :
				SomolIsAdvancedPin(InPin->Properties) ? TEXT("advanced_optional") :
				SomolIsOverrideOrUserParamPin(InPin->Properties) ? TEXT("override_or_user_param_optional") :
				TEXT("optional_or_pass_through"));
			PinJson->SetBoolField(TEXT("has_default_value"), false);
			PinJson->SetStringField(TEXT("default_value"), TEXT(""));
			PinJsonArray.Add(MakeShared<FJsonValueObject>(PinJson));

			if (bRequired && EdgeCount == 0)
			{
				MissingRequiredJson.Add(MakeShared<FJsonValueObject>(PinJson));
			}
		}
		NodeJson->SetArrayField(TEXT("input_pins"), PinJsonArray);
		NodesJson.Add(MakeShared<FJsonValueObject>(NodeJson));
	}

	Audit->SetArrayField(TEXT("nodes"), NodesJson);
	Audit->SetNumberField(TEXT("required_pin_count"), RequiredCount);
	Audit->SetNumberField(TEXT("optional_pin_count"), OptionalCount);
	Audit->SetNumberField(TEXT("missing_required_count"), MissingRequiredCount);
	Audit->SetNumberField(TEXT("connected_required_count"), ConnectedRequiredCount);
	Audit->SetArrayField(TEXT("missing_required_pins"), MissingRequiredJson);
	Audit->SetBoolField(TEXT("passed"), MissingRequiredCount == 0);
	return Audit;
}

// ═══════════════════════════════════════════════════════════════════════════════
// A1: pcg_graph_validate — pre-execution graph validation
// Checks:
//   - dangling required input pins
//   - settings CDO missing
//   - pin type mismatches on edges
//   - graph has at least one leaf output (warning only)
//   - circular reference via subgraph (best-effort detection)
// ═══════════════════════════════════════════════════════════════════════════════
static bool Tool_PcgGraphValidate(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	UPCGGraph* Graph = LoadPCGGraph(Context.Services, AssetPath, OutError);
	if (!Graph) return false;

	FString Strictness = TEXT("error");
	Arguments->TryGetStringField(TEXT("strictness"), Strictness);
	if (!Strictness.Equals(TEXT("warn"), ESearchCase::IgnoreCase)
		&& !Strictness.Equals(TEXT("error"), ESearchCase::IgnoreCase)
		&& !Strictness.Equals(TEXT("permissive"), ESearchCase::IgnoreCase))
	{
		Strictness = TEXT("error");
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 ErrorCount = 0, WarningCount = 0;

	auto AddIssue = [&](const FString& Severity, const FString& Code, const FString& NodeName,
		const FString& PinLabel, const FString& Message, const FString& Hint)
	{
		AddValidationIssue(Issues, ErrorCount, WarningCount, AssetPath, Severity, Code, NodeName, PinLabel, Message, Hint, Strictness);
	};

	const TArray<UPCGNode*>& Nodes = Graph->GetNodes();
	if (Nodes.Num() == 0)
	{
		AddIssue(TEXT("warning"), TEXT("empty_graph"), TEXT(""), TEXT(""),
			TEXT("Graph has no nodes."),
			TEXT("Add at least one Surface Sampler or Landscape Sampler, then a spawn node."));
	}

	// Build a quick index of node pointers seen for cross-validation.
	TSet<const UPCGNode*> SeenNodes;
	for (UPCGNode* N : Nodes) { if (N) SeenNodes.Add(N); }

	for (UPCGNode* Node : Nodes)
	{
		if (!Node)
		{
			AddIssue(TEXT("error"), TEXT("null_node"), TEXT(""), TEXT(""),
				TEXT("Graph contains a null node reference."),
				TEXT("Remove the null slot via pcg_graph_remove_node."));
			continue;
		}

		const FString NodeName = Node->GetName();
		const UPCGSettings* Settings = Node->GetSettings();
		if (!Settings)
		{
			AddIssue(TEXT("error"), TEXT("settings_missing"), NodeName, TEXT(""),
				TEXT("Node has no Settings CDO."),
				TEXT("Use pcg_node_catalog to find a valid settings class and re-add the node."));
			continue;
		}

		// Check input pins. UE 5.7 exposes real Required/Advanced status; only
		// required-by-execution pins are blocking when disconnected.
		for (UPCGPin* InPin : Node->GetInputPins())
		{
			if (!InPin) continue;
			const int32 EdgeCount = InPin->Edges.Num();
			const bool bPinUsed = Node->IsPinUsedByNodeExecution(InPin);
			const bool bRequired = bPinUsed && SomolInputPinRequiredByExecution(Node, InPin);
			if (EdgeCount == 0)
			{
				AddIssue(
					bRequired ? TEXT("error") : TEXT("warning"),
					bRequired ? TEXT("required_input_unconnected") : TEXT("optional_input_unconnected"),
					NodeName, InPin->Properties.Label.ToString(),
					FString::Printf(TEXT("Input pin '%s' has no incoming edges."), *InPin->Properties.Label.ToString()),
					bRequired
						? TEXT("Connect a compatible upstream output before generate. Use pcg_node_catalog_lookup or pcg_pin_compat_validate.")
						: TEXT("Optional or advanced input is unconnected; this is not a blocking generate error."));
				continue;
			}

			// Type compatibility check per edge.
			for (const TObjectPtr<UPCGEdge>& Edge : InPin->Edges)
			{
				if (!Edge)
				{
					AddIssue(TEXT("error"), TEXT("edge_null"), NodeName, InPin->Properties.Label.ToString(),
						TEXT("Input pin contains a null edge reference."),
						TEXT("Disconnect and reconnect via pcg_graph_connect / pcg_graph_disconnect."));
					continue;
				}
				UPCGPin* UpstreamPin = Edge->InputPin.Get();
				if (!UpstreamPin)
				{
					AddIssue(TEXT("error"), TEXT("edge_broken"), NodeName, InPin->Properties.Label.ToString(),
						TEXT("Incoming edge points to a null upstream pin."),
						TEXT("Disconnect and reconnect via pcg_graph_connect / pcg_graph_disconnect."));
					continue;
				}

				if (!SeenNodes.Contains(UpstreamPin->Node.Get()) || !SeenNodes.Contains(InPin->Node.Get()))
				{
					AddIssue(TEXT("error"), TEXT("edge_cross_graph"), NodeName, InPin->Properties.Label.ToString(),
						TEXT("Edge endpoint belongs to a node outside this graph."),
						TEXT("Remove the stale edge and reconnect pins that are both owned by the same graph."));
				}

				TSharedRef<FJsonObject> Compat = MakeShared<FJsonObject>();
				const bool bCompat = ArePinsCompatible(UpstreamPin, InPin, Compat, /*bCheckTargetCapacity=*/false);
				if (!bCompat)
				{
					FString CompatMessage;
					Compat->TryGetStringField(TEXT("message"), CompatMessage);
					AddIssue(TEXT("error"), TEXT("pin_type_mismatch"),
						NodeName, InPin->Properties.Label.ToString(),
						FString::Printf(TEXT("Pin compatibility failed: upstream '%s' outputs %s, downstream expects %s. %s"),
							UpstreamPin->Node ? *NodeDisplayName(UpstreamPin->Node) : TEXT("x"),
							*CurrentPinTypeToString(UpstreamPin),
							*PinTypeToString(InPin->Properties.AllowedTypes),
							*CompatMessage),
						TEXT("Insert a compatible transform node (e.g. PointFromSurface, SplineSampler), or change the downstream node."));
				}
			}
		}

		// Detect dangling outputs: emit warning only.
		int32 TotalOutEdges = 0;
		for (UPCGPin* OutPin : Node->GetOutputPins())
		{
			if (OutPin) TotalOutEdges += OutPin->Edges.Num();
		}
		if (TotalOutEdges == 0 && Node->GetOutputPins().Num() > 0)
		{
			// It's okay for the final spawn node to have no downstream edges, so downgrade to info.
			const FString Cat = CategorizeSettings(Settings->GetClass());
			if (Cat != TEXT("spawn") && Cat != TEXT("debug"))
			{
				AddIssue(TEXT("warning"), TEXT("output_unused"), NodeName, TEXT(""),
					TEXT("No downstream consumer for this node's outputs."),
					TEXT("Connect it to a spawn/debug node, or remove it if unused."));
			}
		}
	}

	TArray<TArray<const UPCGNode*>> Cycles;
	CollectGraphCycles(Graph, Cycles);
	for (const TArray<const UPCGNode*>& Cycle : Cycles)
	{
		TArray<FString> Names;
		for (const UPCGNode* CycleNode : Cycle)
		{
			Names.Add(NodeDisplayName(CycleNode));
		}
		AddIssue(TEXT("error"), TEXT("graph_cycle"), Names.Num() > 0 ? Names[0] : TEXT(""), TEXT(""),
			FString::Printf(TEXT("PCG graph contains a directed cycle: %s"), *FString::Join(Names, TEXT(" -> "))),
			TEXT("Break one feedback edge or replace the loop with an explicit PCG loop/subgraph pattern before generate."));
	}

	TSharedRef<FJsonObject> RequiredAudit = BuildRequiredPinAuditJson(Graph);

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("dry_run_receipt_schema"), TEXT("somol.pcg.dry_run.heuristic_receipt.v1"));
	OutStructured->SetStringField(TEXT("dry_run_status"), TEXT("heuristic_estimate_only"));
	OutStructured->SetStringField(TEXT("estimation_method"), TEXT("graph_node_category_heuristic"));
	OutStructured->SetStringField(TEXT("estimation_confidence"), TEXT("low"));
	OutStructured->SetNumberField(TEXT("estimated_accuracy_tolerance_ratio"), 0.5);
	OutStructured->SetBoolField(TEXT("exact_count_available"), false);
	OutStructured->SetBoolField(TEXT("mutation_attempted"), false);
	OutStructured->SetBoolField(TEXT("generation_dispatched"), false);
	OutStructured->SetBoolField(TEXT("requires_post_generate_calibration"), true);
	OutStructured->SetStringField(TEXT("receipt_gate_status"), TEXT("budget_screen_only"));
	OutStructured->SetStringField(TEXT("next_required_receipt"), TEXT("pcg_generate plus pcg_spawned_actor_index/readback for exact spawned counts"));
	OutStructured->SetStringField(TEXT("strictness"), Strictness);
	OutStructured->SetNumberField(TEXT("node_count"), Nodes.Num());
	OutStructured->SetNumberField(TEXT("error_count"), ErrorCount);
	OutStructured->SetNumberField(TEXT("warning_count"), WarningCount);
	OutStructured->SetArrayField(TEXT("issues"), Issues);
	OutStructured->SetObjectField(TEXT("required_pin_audit"), RequiredAudit);
	OutStructured->SetArrayField(TEXT("cycles"), CyclesToJson(Cycles));
	OutStructured->SetNumberField(TEXT("cycle_count"), Cycles.Num());
	OutStructured->SetBoolField(TEXT("passed"), ErrorCount == 0);

	OutSummary = FString::Printf(TEXT("Validate '%s': %d nodes, %d errors, %d warnings — %s"),
		*AssetPath, Nodes.Num(), ErrorCount, WarningCount,
		ErrorCount == 0 ? TEXT("PASSED") : TEXT("FAILED"));
	if (ErrorCount > 0)
	{
		SololmcpError::Set(OutStructured, TEXT("VALIDATION_FAILED"), TEXT("asset_path"),
			TEXT("PCG graph validation found blocking errors; do not generate until they are fixed."));
		OutError = OutSummary;
		return false;
	}
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Direction 4: pcg_pin_compat_validate
// ═══════════════════════════════════════════════════════════════════════════════
static bool Tool_PcgPinCompatValidate(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& OutError)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	UPCGGraph* Graph = LoadPCGGraph(Context.Services, AssetPath, OutError);
	if (!Graph) return false;

	UPCGNode* SourceNode = nullptr;
	UPCGPin* SourcePin = nullptr;
	if (!TryResolvePinReference(Graph, TEXT("source_node"), TEXT("source_pin"), TEXT("source_pin_path"),
		/*bOutputPin=*/true, Arguments, SourceNode, SourcePin, OutError))
	{
		return false;
	}

	UPCGNode* TargetNode = nullptr;
	UPCGPin* TargetPin = nullptr;
	if (!TryResolvePinReference(Graph, TEXT("target_node"), TEXT("target_pin"), TEXT("target_pin_path"),
		/*bOutputPin=*/false, Arguments, TargetNode, TargetPin, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Compat = MakeShared<FJsonObject>();
	const bool bCompatible = ArePinsCompatible(SourcePin, TargetPin, Compat, /*bCheckTargetCapacity=*/true);

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetObjectField(TEXT("compatibility_result"), Compat);
	OutStructured->SetBoolField(TEXT("compatible"), bCompatible);
	OutStructured->SetBoolField(TEXT("passed"), bCompatible);
	OutStructured->SetStringField(TEXT("source"), FString::Printf(TEXT("%s::%s"),
		*NodeDisplayName(SourceNode), *SourcePin->Properties.Label.ToString()));
	OutStructured->SetStringField(TEXT("target"), FString::Printf(TEXT("%s::%s"),
		*NodeDisplayName(TargetNode), *TargetPin->Properties.Label.ToString()));

	OutSummary = FString::Printf(TEXT("Pin compatibility %s -> %s: %s"),
		*OutStructured->GetStringField(TEXT("source")),
		*OutStructured->GetStringField(TEXT("target")),
		bCompatible ? TEXT("compatible") : TEXT("not compatible"));
	if (!bCompatible)
	{
		OutError = OutSummary;
	}
	return bCompatible;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Direction 4: pcg_graph_cycle_detect
// ═══════════════════════════════════════════════════════════════════════════════
static bool Tool_PcgGraphCycleDetect(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& OutError)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	UPCGGraph* Graph = LoadPCGGraph(Context.Services, AssetPath, OutError);
	if (!Graph) return false;

	TArray<TArray<const UPCGNode*>> Cycles;
	CollectGraphCycles(Graph, Cycles);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());
	OutStructured->SetNumberField(TEXT("cycle_count"), Cycles.Num());
	OutStructured->SetBoolField(TEXT("cycle_free"), Cycles.Num() == 0);
	OutStructured->SetBoolField(TEXT("passed"), Cycles.Num() == 0);
	OutStructured->SetArrayField(TEXT("cycles"), CyclesToJson(Cycles));
	OutSummary = FString::Printf(TEXT("Cycle detect '%s': %d cycle(s)."), *AssetPath, Cycles.Num());
	if (Cycles.Num() > 0)
	{
		OutError = OutSummary;
		return false;
	}
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Direction 4: pcg_graph_required_pin_audit
// ═══════════════════════════════════════════════════════════════════════════════
static bool Tool_PcgGraphRequiredPinAudit(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& OutError)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	UPCGGraph* Graph = LoadPCGGraph(Context.Services, AssetPath, OutError);
	if (!Graph) return false;

	TSharedRef<FJsonObject> Audit = BuildRequiredPinAuditJson(Graph);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetObjectField(TEXT("audit"), Audit);
	OutStructured->SetNumberField(TEXT("missing_required_count"), Audit->GetNumberField(TEXT("missing_required_count")));
	OutStructured->SetBoolField(TEXT("passed"), Audit->GetBoolField(TEXT("passed")));
	OutSummary = FString::Printf(TEXT("Required pin audit '%s': %.0f missing required pin(s)."),
		*AssetPath, Audit->GetNumberField(TEXT("missing_required_count")));
	if (!Audit->GetBoolField(TEXT("passed")))
	{
		OutError = OutSummary;
		return false;
	}
	return true;
}

static FString ClassifyFailureCategory(const FString& FailureText, const TArray<TSharedPtr<FJsonValue>>* Issues, double& OutConfidence, TArray<FString>& OutSignals)
{
	FString Text = FailureText.ToLower();
	if (Issues)
	{
		for (const TSharedPtr<FJsonValue>& IssueValue : *Issues)
		{
			const TSharedPtr<FJsonObject> Issue = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
			if (!Issue.IsValid()) continue;
			FString Code;
			FString Message;
			Issue->TryGetStringField(TEXT("code"), Code);
			Issue->TryGetStringField(TEXT("message"), Message);
			Text += TEXT(" ");
			Text += Code.ToLower();
			Text += TEXT(" ");
			Text += Message.ToLower();
		}
	}

	auto HasAny = [&](std::initializer_list<const TCHAR*> Needles) -> bool
	{
		for (const TCHAR* Needle : Needles)
		{
			if (Text.Contains(Needle, ESearchCase::IgnoreCase))
			{
				OutSignals.Add(Needle);
				return true;
			}
		}
		return false;
	};

	if (HasAny({TEXT("pin_type_mismatch"), TEXT("required_input_unconnected"), TEXT("validation"), TEXT("cycle"), TEXT("edge_broken"), TEXT("not compatible"), TEXT("missing required")}))
	{
		OutConfidence = 0.92;
		return TEXT("validation");
	}
	if (HasAny({TEXT("budget"), TEXT("over_budget"), TEXT("too many points"), TEXT("estimated_spawned_points"), TEXT("tile cap"), TEXT("density")}))
	{
		OutConfidence = 0.86;
		return TEXT("budget");
	}
	if (HasAny({TEXT("asset_missing"), TEXT("missing mesh"), TEXT("missing material"), TEXT("could not load"), TEXT("failed to load"), TEXT("asset path"), TEXT("not found")}))
	{
		OutConfidence = 0.84;
		return TEXT("asset_missing");
	}
	if (HasAny({TEXT("streaming"), TEXT("world partition"), TEXT("level not loaded"), TEXT("unloaded"), TEXT("streaming unavailable")}))
	{
		OutConfidence = 0.82;
		return TEXT("streaming");
	}
	if (HasAny({TEXT("index_stale"), TEXT("stale actor index"), TEXT("actor index"), TEXT("durable binding"), TEXT("not indexed")}))
	{
		OutConfidence = 0.8;
		return TEXT("index_stale");
	}
	if (HasAny({TEXT("generate"), TEXT("generation"), TEXT("pcg_generate"), TEXT("executor"), TEXT("failed to generate")}))
	{
		OutConfidence = 0.66;
		return TEXT("generation");
	}

	OutConfidence = 0.35;
	OutSignals.Add(TEXT("no_strong_signal"));
	return TEXT("unknown");
}

static void AddRepairCandidate(TArray<TSharedPtr<FJsonValue>>& Candidates, const FString& Action, const FString& Risk, const TArray<FString>& Tools, const FString& Rationale)
{
	TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("action"), Action);
	Candidate->SetStringField(TEXT("risk_level"), Risk);
	Candidate->SetArrayField(TEXT("mutating_tools"), StringArrayJson(Tools));
	Candidate->SetStringField(TEXT("rationale"), Rationale);
	Candidate->SetStringField(TEXT("rollback_path"), TEXT("pcg_graph_snapshot before mutation; pcg_graph_restore on failed validate/dry_run/diff."));
	Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Direction 9: pcg_failure_classify
// ═══════════════════════════════════════════════════════════════════════════════
static bool Tool_PcgFailureClassify(
	const FSololmcpToolExecutionContext& /*Context*/,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& /*OutError*/)
{
	FString FailureText;
	Arguments->TryGetStringField(TEXT("failure_text"), FailureText);
	FString ErrorText;
	if (Arguments->TryGetStringField(TEXT("error"), ErrorText) && !ErrorText.IsEmpty())
	{
		FailureText += TEXT(" ");
		FailureText += ErrorText;
	}
	FString ToolName;
	Arguments->TryGetStringField(TEXT("tool_name"), ToolName);
	if (!ToolName.IsEmpty())
	{
		FailureText += TEXT(" ");
		FailureText += ToolName;
	}

	const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
	Arguments->TryGetArrayField(TEXT("issues"), Issues);

	TArray<FString> Signals;
	double Confidence = 0.0;
	const FString Category = ClassifyFailureCategory(FailureText, Issues, Confidence, Signals);

	TArray<TSharedPtr<FJsonValue>> Candidates;
	if (Category == TEXT("validation"))
	{
		AddRepairCandidate(Candidates, TEXT("Run pcg_graph_auto_repair_plan, then fix required pins, incompatible edges, or cycles before generate."),
			TEXT("medium"), {TEXT("pcg_graph_snapshot"), TEXT("pcg_graph_connect"), TEXT("pcg_graph_disconnect")},
			TEXT("Validation failures are deterministic graph-shape problems and should be repaired before any generation retry."));
	}
	else if (Category == TEXT("budget"))
	{
		AddRepairCandidate(Candidates, TEXT("Lower density, split tiles, or enforce <=4 tile generate batches before retry."),
			TEXT("low"), {TEXT("pcg_generation_budget_set"), TEXT("pcg_partition_preview"), TEXT("pcg_dry_run")},
			TEXT("Budget failures should be fixed by reducing scope or density, not by blind retry."));
	}
	else if (Category == TEXT("asset_missing"))
	{
		AddRepairCandidate(Candidates, TEXT("Resolve missing mesh/material paths and substitute known-good assets before validate/generate."),
			TEXT("medium"), {TEXT("asset_search"), TEXT("pcg_graph_set_node_property")},
			TEXT("Missing assets usually require property replacement on a spawner or material slot."));
	}
	else if (Category == TEXT("streaming"))
	{
		AddRepairCandidate(Candidates, TEXT("Load the required level/cell or retarget the PCG volume to a loaded world partition scope."),
			TEXT("medium"), {TEXT("world_partition_status_lite"), TEXT("level_streaming_status")},
			TEXT("Streaming failures are environment/scope failures; retry only after level availability is proven."));
	}
	else if (Category == TEXT("index_stale"))
	{
		AddRepairCandidate(Candidates, TEXT("Refresh spawned actor index and reconcile durable tags before animation/camera follow-up."),
			TEXT("low"), {TEXT("pcg_spawned_actor_index")},
			TEXT("Stale index failures need a read refresh before any assignment replay."));
	}
	else
	{
		AddRepairCandidate(Candidates, TEXT("Collect graph validate, dry-run, component info, and spawned actor index evidence before retry."),
			TEXT("low"), {TEXT("pcg_graph_validate"), TEXT("pcg_dry_run"), TEXT("pcg_component_info"), TEXT("pcg_spawned_actor_index")},
			TEXT("No single failure band is dominant; gather structured evidence first."));
	}

	OutStructured->SetBoolField(TEXT("ok"), true);
	OutStructured->SetStringField(TEXT("failure_category"), Category);
	OutStructured->SetStringField(TEXT("failure_band"), Category);
	OutStructured->SetNumberField(TEXT("confidence"), Confidence);
	OutStructured->SetArrayField(TEXT("signals"), StringArrayJson(Signals));
	OutStructured->SetArrayField(TEXT("repair_candidates"), Candidates);
	OutStructured->SetStringField(TEXT("next_recommended_tool"),
		Category == TEXT("validation") ? TEXT("pcg_graph_auto_repair_plan") :
		Category == TEXT("budget") ? TEXT("pcg_partition_preview") :
		Category == TEXT("asset_missing") ? TEXT("asset_search") :
		Category == TEXT("index_stale") ? TEXT("pcg_spawned_actor_index") :
		TEXT("pcg_graph_validate"));
	OutSummary = FString::Printf(TEXT("PCG failure classified as %s (confidence %.2f)."), *Category, Confidence);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Direction 9: pcg_graph_auto_repair_plan — read-only plan
// ═══════════════════════════════════════════════════════════════════════════════
static bool Tool_PcgGraphAutoRepairPlan(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& /*OutError*/)
{
	FString AssetPath;
	Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
	FString FailureText;
	Arguments->TryGetStringField(TEXT("failure_text"), FailureText);

	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> Cycles;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	if (!AssetPath.IsEmpty())
	{
		TSharedRef<FJsonObject> ValidateArgs = MakeShared<FJsonObject>();
		ValidateArgs->SetStringField(TEXT("asset_path"), AssetPath);
		ValidateArgs->SetStringField(TEXT("strictness"), TEXT("error"));
		TSharedRef<FJsonObject> ValidateOut = MakeShared<FJsonObject>();
		FString ValidateSummary;
		FString ValidateError;
		Tool_PcgGraphValidate(Context, ValidateArgs, ValidateOut, ValidateSummary, ValidateError);
		const TArray<TSharedPtr<FJsonValue>>* ValidateIssues = nullptr;
		if (ValidateOut->TryGetArrayField(TEXT("issues"), ValidateIssues) && ValidateIssues)
		{
			Issues = *ValidateIssues;
		}
		const TArray<TSharedPtr<FJsonValue>>* ValidateCycles = nullptr;
		if (ValidateOut->TryGetArrayField(TEXT("cycles"), ValidateCycles) && ValidateCycles)
		{
			Cycles = *ValidateCycles;
		}
		ErrorCount = static_cast<int32>(ValidateOut->GetNumberField(TEXT("error_count")));
		WarningCount = static_cast<int32>(ValidateOut->GetNumberField(TEXT("warning_count")));
		FailureText += TEXT(" ");
		FailureText += ValidateSummary;
		FailureText += TEXT(" ");
		FailureText += ValidateError;
	}

	TArray<FString> Signals;
	double Confidence = 0.0;
	const FString Category = ClassifyFailureCategory(FailureText, &Issues, Confidence, Signals);

	TArray<TSharedPtr<FJsonValue>> Steps;
	TSet<FString> RecommendedTools;
	auto AddStep = [&](const FString& IssueId, const FString& Title, const FString& Rationale, const FString& Risk, const TArray<FString>& ReadTools, const TArray<FString>& MutatingTools)
	{
		TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetNumberField(TEXT("order"), Steps.Num() + 1);
		Step->SetStringField(TEXT("issue_id"), IssueId);
		Step->SetStringField(TEXT("title"), Title);
		Step->SetStringField(TEXT("rationale"), Rationale);
		Step->SetStringField(TEXT("risk_level"), Risk);
		Step->SetArrayField(TEXT("read_tools"), StringArrayJson(ReadTools));
		Step->SetArrayField(TEXT("mutating_tools"), StringArrayJson(MutatingTools));
		Step->SetStringField(TEXT("rollback_path"), TEXT("Take pcg_graph_snapshot before mutation; restore if validate/dry_run/diff fails."));
		Step->SetBoolField(TEXT("plan_only"), true);
		Steps.Add(MakeShared<FJsonValueObject>(Step));
		for (const FString& Tool : ReadTools) RecommendedTools.Add(Tool);
		for (const FString& Tool : MutatingTools) RecommendedTools.Add(Tool);
	};

	for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
	{
		const TSharedPtr<FJsonObject> Issue = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
		if (!Issue.IsValid()) continue;
		FString Code;
		FString IssueId;
		FString Node;
		FString Pin;
		Issue->TryGetStringField(TEXT("code"), Code);
		Issue->TryGetStringField(TEXT("issue_id"), IssueId);
		Issue->TryGetStringField(TEXT("node"), Node);
		Issue->TryGetStringField(TEXT("pin"), Pin);
		if (Code == TEXT("required_input_unconnected"))
		{
			AddStep(IssueId,
				FString::Printf(TEXT("Connect required input %s::%s"), *Node, *Pin),
				TEXT("A required execution pin has no upstream data; generation can be culled or fail."),
				TEXT("medium"),
				{TEXT("pcg_node_catalog_lookup"), TEXT("pcg_pin_compat_validate"), TEXT("pcg_graph_required_pin_audit")},
				{TEXT("pcg_graph_connect")});
		}
		else if (Code == TEXT("pin_type_mismatch"))
		{
			AddStep(IssueId,
				FString::Printf(TEXT("Repair incompatible edge into %s::%s"), *Node, *Pin),
				TEXT("Current source output type does not satisfy the target pin. Insert a conversion/filter node or reconnect to a compatible output."),
				TEXT("medium"),
				{TEXT("pcg_node_catalog_lookup"), TEXT("pcg_pin_compat_validate")},
				{TEXT("pcg_graph_disconnect"), TEXT("pcg_graph_add_node"), TEXT("pcg_graph_connect")});
		}
		else if (Code == TEXT("graph_cycle"))
		{
			AddStep(IssueId,
				TEXT("Break directed PCG graph cycle"),
				TEXT("Directed cycles can trap generation/validation. Remove one back-edge or model the loop with supported PCG loop/subgraph pins."),
				TEXT("high"),
				{TEXT("pcg_graph_cycle_detect"), TEXT("pcg_graph_explain")},
				{TEXT("pcg_graph_disconnect")});
		}
		else if (Code == TEXT("edge_cross_graph") || Code == TEXT("edge_broken") || Code == TEXT("edge_null"))
		{
			AddStep(IssueId,
				FString::Printf(TEXT("Rebuild stale edge at %s::%s"), *Node, *Pin),
				TEXT("The edge object references null or wrong-graph endpoints; rebuild it inside the current graph."),
				TEXT("medium"),
				{TEXT("pcg_pin_compat_validate")},
				{TEXT("pcg_graph_disconnect"), TEXT("pcg_graph_connect")});
		}
		else if (Code == TEXT("settings_missing") || Code == TEXT("null_node"))
		{
			AddStep(IssueId,
				FString::Printf(TEXT("Replace invalid node %s"), *Node),
				TEXT("A node has no valid settings object. Recreate it from catalog instead of generating through it."),
				TEXT("medium"),
				{TEXT("pcg_node_catalog_lookup")},
				{TEXT("pcg_graph_remove_node"), TEXT("pcg_graph_add_node")});
		}
		else if (Code == TEXT("output_unused"))
		{
			AddStep(IssueId,
				FString::Printf(TEXT("Review unused output on %s"), *Node),
				TEXT("Unused non-terminal output may be harmless, but often indicates a missed connection in generated graphs."),
				TEXT("low"),
				{TEXT("pcg_graph_explain")},
				{TEXT("pcg_graph_connect"), TEXT("pcg_graph_remove_node")});
		}
	}

	if (Category == TEXT("budget"))
	{
		AddStep(TEXT("budget_repair"),
			TEXT("Reduce PCG generation budget pressure"),
			TEXT("Plan density reduction, tile split, or LOD/static mesh simplification before retry."),
			TEXT("low"),
			{TEXT("pcg_partition_preview"), TEXT("pcg_dry_run"), TEXT("pcg_generation_budget_get")},
			{TEXT("pcg_generation_budget_set"), TEXT("pcg_graph_set_node_property")});
	}
	else if (Category == TEXT("asset_missing"))
	{
		AddStep(TEXT("asset_missing_repair"),
			TEXT("Substitute missing mesh/material references"),
			TEXT("Find a valid replacement asset and update the relevant spawner/material property."),
			TEXT("medium"),
			{TEXT("asset_search"), TEXT("pcg_graph_explain")},
			{TEXT("pcg_graph_set_node_property")});
	}

	if (Steps.IsEmpty())
	{
		AddStep(TEXT("collect_more_evidence"),
			TEXT("Collect structured PCG evidence"),
			TEXT("No direct graph repair was inferred. Validate, dry-run, inspect component state, then classify again."),
			TEXT("low"),
			{TEXT("pcg_graph_validate"), TEXT("pcg_dry_run"), TEXT("pcg_component_info")},
			{});
	}

	TArray<FString> ToolList = RecommendedTools.Array();
	ToolList.Sort();

	OutStructured->SetBoolField(TEXT("ok"), true);
	OutStructured->SetBoolField(TEXT("plan_only"), true);
	OutStructured->SetBoolField(TEXT("can_apply"), false);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("failure_category"), Category);
	OutStructured->SetNumberField(TEXT("confidence"), Confidence);
	OutStructured->SetNumberField(TEXT("validation_error_count"), ErrorCount);
	OutStructured->SetNumberField(TEXT("validation_warning_count"), WarningCount);
	OutStructured->SetNumberField(TEXT("cycle_count"), Cycles.Num());
	OutStructured->SetArrayField(TEXT("signals"), StringArrayJson(Signals));
	OutStructured->SetArrayField(TEXT("issues"), Issues);
	OutStructured->SetArrayField(TEXT("cycles"), Cycles);
	OutStructured->SetArrayField(TEXT("repair_steps"), Steps);
	OutStructured->SetArrayField(TEXT("recommended_tools"), StringArrayJson(ToolList));
	OutStructured->SetStringField(TEXT("risk_level"),
		Category == TEXT("validation") && Cycles.Num() > 0 ? TEXT("high") :
		Category == TEXT("asset_missing") ? TEXT("medium") :
		TEXT("medium"));
	OutStructured->SetStringField(TEXT("rollback_path"), TEXT("pcg_graph_snapshot -> apply one patch -> pcg_graph_validate -> pcg_dry_run -> pcg_graph_diff -> restore on failure."));
	OutSummary = FString::Printf(TEXT("PCG auto repair plan for '%s': %d step(s), category=%s."),
		AssetPath.IsEmpty() ? TEXT("<no graph>") : *AssetPath, Steps.Num(), *Category);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// A3: pcg_graph_explain — human-readable semantic description
// Produces a "chain" string per output-terminal path, e.g.:
//   "LandscapeSampler[Land] -> DensityByTag[<0.3] -> SurfaceSampler -> SpawnStaticMesh[Pine]"
// ═══════════════════════════════════════════════════════════════════════════════
static FString BuildChainString(const UPCGNode* Node, TSet<const UPCGNode*>& Visited, int32 Depth = 0)
{
	if (!Node || Visited.Contains(Node) || Depth > 64) return TEXT("...");
	Visited.Add(Node);

	FString ClassShort = TEXT("Node");
	if (const UPCGSettings* S = Node->GetSettings())
	{
		ClassShort = S->GetClass()->GetName();
		ClassShort.RemoveFromStart(TEXT("PCGSettings_"));
		ClassShort.RemoveFromStart(TEXT("UPCG"));
	}

	// Recursively walk back through primary input.
	FString Upstream;
	for (UPCGPin* InPin : Node->GetInputPins())
	{
		if (!InPin || InPin->Edges.Num() == 0) continue;
		for (const TObjectPtr<UPCGEdge>& Edge : InPin->Edges)
		{
			if (!Edge || !Edge->InputPin.Get() || !Edge->InputPin->Node) continue;
			Upstream = BuildChainString(Edge->InputPin->Node, Visited, Depth + 1);
			break;
		}
		if (!Upstream.IsEmpty()) break;
	}

	if (Upstream.IsEmpty()) return ClassShort;
	return Upstream + TEXT(" -> ") + ClassShort;
}

static bool Tool_PcgGraphExplain(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}
	UPCGGraph* Graph = LoadPCGGraph(Context.Services, AssetPath, OutError);
	if (!Graph) return false;

	// Find terminal (no downstream edges) nodes as chain endpoints.
	TArray<TSharedPtr<FJsonValue>> Chains;
	for (UPCGNode* Node : Graph->GetNodes())
	{
		if (!Node) continue;
		int32 DownstreamEdges = 0;
		for (UPCGPin* OutPin : Node->GetOutputPins()) { if (OutPin) DownstreamEdges += OutPin->Edges.Num(); }
		if (DownstreamEdges > 0) continue;

		TSet<const UPCGNode*> Visited;
		const FString Chain = BuildChainString(Node, Visited);
		TSharedRef<FJsonObject> Co = MakeShared<FJsonObject>();
		Co->SetStringField(TEXT("terminal"), Node->GetName());
		Co->SetStringField(TEXT("chain"), Chain);
		Co->SetNumberField(TEXT("visited_count"), Visited.Num());
		Chains.Add(MakeShared<FJsonValueObject>(Co));
	}

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());
	OutStructured->SetArrayField(TEXT("chains"), Chains);
	OutSummary = FString::Printf(TEXT("Explain '%s': %d nodes, %d terminal chains"),
		*AssetPath, Graph->GetNodes().Num(), Chains.Num());
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// A9: pcg_dry_run — skeletal count-only estimator.
// Stage 1: uses heuristics based on node types + graph structure to estimate
// point counts, without actually invoking PCG generation. This gives the AI a
// cheap "will this blow my budget" signal before a real generate.
//
// TODO Stage 2: hook into PCGSubsystem with a CountOnly debug mode.
// ═══════════════════════════════════════════════════════════════════════════════
static bool Tool_PcgDryRun(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}
	double AreaMeters2 = 10000.0; // Default: 100x100m tile
	Arguments->TryGetNumberField(TEXT("area_m2"), AreaMeters2);
	double DensityPerM2 = 0.2; // Default assumption
	Arguments->TryGetNumberField(TEXT("default_density_per_m2"), DensityPerM2);

	UPCGGraph* Graph = LoadPCGGraph(Context.Services, AssetPath, OutError);
	if (!Graph) return false;

	// Heuristic: count sampler nodes + average filter survival rates, apply density.
	int32 SamplerCount = 0, FilterCount = 0, SpawnCount = 0;
	double SurvivalRate = 1.0;

	for (UPCGNode* Node : Graph->GetNodes())
	{
		if (!Node || !Node->GetSettings()) continue;
		const FString Cat = CategorizeSettings(Node->GetSettings()->GetClass());
		if (Cat == TEXT("sampling") || Cat == TEXT("landscape") || Cat == TEXT("spline")) ++SamplerCount;
		else if (Cat == TEXT("filter") || Cat == TEXT("density")) { ++FilterCount; SurvivalRate *= 0.6; /* assume 60% pass-through */ }
		else if (Cat == TEXT("spawn")) ++SpawnCount;
	}

	const double RawPoints = FMath::Max(1.0, AreaMeters2 * DensityPerM2 * FMath::Max(1, SamplerCount));
	const double EstimatedSpawned = RawPoints * SurvivalRate;

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("graph_category"), PcgExecutionSafety::InferGraphCategoryFromText(AssetPath));
	OutStructured->SetNumberField(TEXT("area_m2"), AreaMeters2);
	OutStructured->SetNumberField(TEXT("default_density_per_m2"), DensityPerM2);
	OutStructured->SetNumberField(TEXT("sampler_node_count"), SamplerCount);
	OutStructured->SetNumberField(TEXT("filter_node_count"), FilterCount);
	OutStructured->SetNumberField(TEXT("spawn_node_count"), SpawnCount);
	OutStructured->SetNumberField(TEXT("estimated_raw_points"), RawPoints);
	OutStructured->SetNumberField(TEXT("estimated_spawned_points"), EstimatedSpawned);
	OutStructured->SetNumberField(TEXT("survival_rate"), SurvivalRate);
	OutStructured->SetBoolField(TEXT("is_heuristic"), true);
	OutStructured->SetStringField(TEXT("note"),
		TEXT("Stage 1 heuristic. Accuracy ±50%. Use for budget screening only; run pcg_generate for exact counts."));

	// v3.7 A11: attach budget status if a budget is set.
	// (Forward declared below; the impl lives in the A11 block but static order is fine — compilers
	// see the definition earlier if both are in the same TU. To keep strict ordering robust, we
	// declare a slim helper prototype here and define AttachBudgetStatus after it.)
	AttachBudgetStatus(OutStructured, AssetPath, EstimatedSpawned);

	OutSummary = FString::Printf(TEXT("DryRun '%s': ~%.0f points on %.0f m^2 (heuristic)"),
		*AssetPath, EstimatedSpawned, AreaMeters2);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// A2: pcg_graph_template_apply — one-shot scenario templates.
// V1 ships 5 templates: forest_deciduous, grass_field, rock_scatter, riverbank,
// alpine_conifer. Each template delegates to existing pcg_graph_add_node /
// pcg_graph_set_node_property / pcg_graph_connect tools via Registry.ExecuteTool
// so behavior stays consistent with hand-authored graphs.
// ═══════════════════════════════════════════════════════════════════════════════
struct FPcgTemplateNode
{
	const TCHAR* Label;
	const TCHAR* ClassPath;
	// Simple key=value scalar properties (parsed to JSON by applier).
	TArray<TPair<FString, FString>> Props;
};

struct FPcgTemplateEdge
{
	const TCHAR* From; // "Label::Pin"
	const TCHAR* To;   // "Label::Pin"
};

struct FPcgTemplateSpec
{
	const TCHAR* Id;
	const TCHAR* Description;
	TArray<FPcgTemplateNode> Nodes;
	TArray<FPcgTemplateEdge> Edges;
};

static const TArray<FPcgTemplateSpec>& GetBuiltinTemplates()
{
	static const TArray<FPcgTemplateSpec> Templates = {
		{
			TEXT("forest_deciduous"),
			TEXT("Sparse deciduous forest: PCG volume source + volume sampler + density filter. Mesh spawn and transform variance are attached by later asset-binding steps."),
			{
				{TEXT("GetVolumeData_0"), TEXT("/Script/PCG.PCGGetVolumeSettings"), {}},
				{TEXT("Sampler_0"),   TEXT("/Script/PCG.PCGVolumeSamplerSettings"), {}},
				{TEXT("Filter_0"),    TEXT("/Script/PCG.PCGDensityFilterSettings"),
					{{TEXT("LowerBound"), TEXT("0.35")}}}
			},
			{
				{TEXT("GetVolumeData_0::Out"), TEXT("Sampler_0::Volume")},
				{TEXT("Sampler_0::Out"),   TEXT("Filter_0::In")}
			}
		},
		{
			TEXT("grass_field"),
			TEXT("Dense grass field: PCG volume source + volume sampler + density filter. Good for plains, meadows."),
			{
				{TEXT("GetVolumeData_0"), TEXT("/Script/PCG.PCGGetVolumeSettings"), {}},
				{TEXT("Sampler_0"),   TEXT("/Script/PCG.PCGVolumeSamplerSettings"), {}},
				{TEXT("Filter_0"),    TEXT("/Script/PCG.PCGDensityFilterSettings"),
					{{TEXT("LowerBound"), TEXT("0.2")}}}
			},
			{
				{TEXT("GetVolumeData_0::Out"), TEXT("Sampler_0::Volume")},
				{TEXT("Sampler_0::Out"), TEXT("Filter_0::In")}
			}
		},
		{
			TEXT("rock_scatter"),
			TEXT("Sparse rock scatter: PCG volume source + volume sampler + high density filter. Mesh spawn and scale variance are attached by later asset-binding steps."),
			{
				{TEXT("GetVolumeData_0"), TEXT("/Script/PCG.PCGGetVolumeSettings"), {}},
				{TEXT("Sampler_0"),   TEXT("/Script/PCG.PCGVolumeSamplerSettings"), {}},
				{TEXT("Filter_0"),    TEXT("/Script/PCG.PCGDensityFilterSettings"),
					{{TEXT("LowerBound"), TEXT("0.7")}}}
			},
			{
				{TEXT("GetVolumeData_0::Out"), TEXT("Sampler_0::Volume")},
				{TEXT("Sampler_0::Out"),   TEXT("Filter_0::In")}
			}
		},
		{
			TEXT("riverbank"),
			TEXT("Spline-driven riverbank: GetSplineData + SplineSampler + DensityFilter. Input actor must provide a SplineComponent; mesh spawn is attached by later asset-binding steps."),
			{
				{TEXT("GetSplineData_0"),  TEXT("/Script/PCG.PCGGetSplineSettings"), {}},
				{TEXT("Sampler_0"), TEXT("/Script/PCG.PCGSplineSamplerSettings"), {}},
				{TEXT("Filter_0"),  TEXT("/Script/PCG.PCGDensityFilterSettings"),
					{{TEXT("LowerBound"), TEXT("0.25")}}}
			},
			{
				{TEXT("GetSplineData_0::Out"),  TEXT("Sampler_0::Spline")},
				{TEXT("Sampler_0::Out"), TEXT("Filter_0::In")}
			}
		},
		{
			TEXT("alpine_conifer"),
			TEXT("Alpine conifer: PCG volume source + sparse volume sampler + high density filter. Mesh spawn and slope/altitude variation are attached by later asset-binding steps."),
			{
				{TEXT("GetVolumeData_0"), TEXT("/Script/PCG.PCGGetVolumeSettings"), {}},
				{TEXT("Sampler_0"),   TEXT("/Script/PCG.PCGVolumeSamplerSettings"), {}},
				{TEXT("Filter_0"),    TEXT("/Script/PCG.PCGDensityFilterSettings"),
					{{TEXT("LowerBound"), TEXT("0.5")}}}
			},
			{
				{TEXT("GetVolumeData_0::Out"), TEXT("Sampler_0::Volume")},
				{TEXT("Sampler_0::Out"),   TEXT("Filter_0::In")}
			}
		}
	};
	return Templates;
}

static const FPcgTemplateSpec* FindTemplate(const FString& Id)
{
	const TArray<FPcgTemplateSpec>& All = GetBuiltinTemplates();
	for (const FPcgTemplateSpec& T : All)
	{
		if (FString(T.Id).Equals(Id, ESearchCase::IgnoreCase)) return &T;
	}
	return nullptr;
}

// Parse a simple scalar string to a JSON value. Accepts "true"/"false", numeric, or string fallback.
static TSharedPtr<FJsonValue> PropStringToJsonValue(const FString& S)
{
	const FString T = S.TrimStartAndEnd();
	if (T.Equals(TEXT("true"), ESearchCase::IgnoreCase)) return MakeShared<FJsonValueBoolean>(true);
	if (T.Equals(TEXT("false"), ESearchCase::IgnoreCase)) return MakeShared<FJsonValueBoolean>(false);
	if (T.IsNumeric()) return MakeShared<FJsonValueNumber>(FCString::Atod(*T));
	return MakeShared<FJsonValueString>(T);
}

static bool Tool_PcgGraphTemplateApply_Impl(
	FSololmcpToolRegistry& Registry,
	const FSololmcpToolExecutionContext& /*Context*/,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString TemplateId;
	if (!Arguments->TryGetStringField(TEXT("template_id"), TemplateId) || TemplateId.IsEmpty())
	{
		OutError = TEXT("Missing required argument: template_id");
		return false;
	}
	FString TargetGraphPath;
	if (!Arguments->TryGetStringField(TEXT("target_graph_path"), TargetGraphPath) || TargetGraphPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: target_graph_path");
		return false;
	}

	const FPcgTemplateSpec* Spec = FindTemplate(TemplateId);
	if (!Spec)
	{
		const TArray<FPcgTemplateSpec>& All = GetBuiltinTemplates();
		TArray<FString> Ids;
		for (const FPcgTemplateSpec& T : All) Ids.Add(T.Id);
		OutError = FString::Printf(TEXT("Unknown template_id '%s'. Available: %s"),
			*TemplateId, *FString::Join(Ids, TEXT(", ")));
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	int32 NodesCreated = 0;
	int32 ConnectionsCreated = 0;
	int32 PropertyFailures = 0;
	int32 ConnectionFailures = 0;

	// 1. Add each node.
	for (const FPcgTemplateNode& Nd : Spec->Nodes)
	{
		TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("asset_path"), TargetGraphPath);
		A->SetStringField(TEXT("node_class_path"), Nd.ClassPath);
		A->SetStringField(TEXT("node_label"), Nd.Label);
		TSharedRef<FJsonObject> StepOut = MakeShared<FJsonObject>();
		FString StepSummary, StepErr;
		if (!Registry.ExecuteTool(TEXT("pcg_graph_add_node"), A, StepOut, StepSummary, StepErr))
		{
			OutError = FString::Printf(TEXT("Failed to add node '%s' (%s): %s"),
				Nd.Label, Nd.ClassPath, *StepErr);
			return false;
		}
		++NodesCreated;

		// 1b. Apply properties if any.
		if (Nd.Props.Num() > 0)
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			for (const TPair<FString, FString>& KV : Nd.Props)
			{
				TSharedPtr<FJsonValue> V = PropStringToJsonValue(KV.Value);
				if (V.IsValid())
				{
					Props->SetField(KV.Key, V);
				}
			}
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), TargetGraphPath);
			P->SetStringField(TEXT("node"), Nd.Label);
			P->SetObjectField(TEXT("properties"), Props);
			TSharedRef<FJsonObject> PropOut = MakeShared<FJsonObject>();
			FString PropSummary, PropErr;
			if (!Registry.ExecuteTool(TEXT("pcg_graph_set_node_property"), P, PropOut, PropSummary, PropErr))
			{
				// Non-fatal: log as warning; the graph still works without the property.
				++PropertyFailures;
				TSharedRef<FJsonObject> W = MakeShared<FJsonObject>();
				W->SetStringField(TEXT("kind"), TEXT("property_apply_failed"));
				W->SetStringField(TEXT("node"), Nd.Label);
				W->SetStringField(TEXT("detail"), PropErr);
				Warnings.Add(MakeShared<FJsonValueObject>(W));
			}
		}
	}

	// 2. Connect edges.
	for (const FPcgTemplateEdge& E : Spec->Edges)
	{
		TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("asset_path"), TargetGraphPath);
		C->SetStringField(TEXT("source_pin_path"), E.From);
		C->SetStringField(TEXT("target_pin_path"), E.To);
		TSharedRef<FJsonObject> StepOut = MakeShared<FJsonObject>();
		FString StepSummary, StepErr;
		if (!Registry.ExecuteTool(TEXT("pcg_graph_connect"), C, StepOut, StepSummary, StepErr))
		{
			++ConnectionFailures;
			TSharedRef<FJsonObject> W = MakeShared<FJsonObject>();
			W->SetStringField(TEXT("kind"), TEXT("connect_failed"));
			W->SetStringField(TEXT("from"), E.From);
			W->SetStringField(TEXT("to"), E.To);
			W->SetStringField(TEXT("detail"), StepErr);
			Warnings.Add(MakeShared<FJsonValueObject>(W));
			continue;
		}
		++ConnectionsCreated;
	}

	OutStructured->SetStringField(TEXT("template_id"), TemplateId);
	OutStructured->SetStringField(TEXT("target_graph_path"), TargetGraphPath);
	OutStructured->SetStringField(TEXT("template_description"), Spec->Description);
	OutStructured->SetNumberField(TEXT("nodes_created"), NodesCreated);
	OutStructured->SetNumberField(TEXT("connections_created"), ConnectionsCreated);
	OutStructured->SetNumberField(TEXT("expected_nodes"), Spec->Nodes.Num());
	OutStructured->SetNumberField(TEXT("expected_connections"), Spec->Edges.Num());
	OutStructured->SetNumberField(TEXT("property_failure_count"), PropertyFailures);
	OutStructured->SetNumberField(TEXT("connection_failure_count"), ConnectionFailures);
	const bool bComplete = NodesCreated == Spec->Nodes.Num() &&
		ConnectionsCreated == Spec->Edges.Num() &&
		PropertyFailures == 0 &&
		ConnectionFailures == 0;
	OutStructured->SetBoolField(TEXT("complete"), bComplete);
	OutStructured->SetBoolField(TEXT("mutation_complete"), bComplete);
	OutStructured->SetBoolField(TEXT("applied"), bComplete);
	OutStructured->SetArrayField(TEXT("warnings"), Warnings);
	OutStructured->SetStringField(TEXT("next_recommended_tool"), TEXT("pcg_graph_validate"));

	if (bComplete)
	{
		TSharedRef<FJsonObject> ValidateArgs = MakeShared<FJsonObject>();
		ValidateArgs->SetStringField(TEXT("asset_path"), TargetGraphPath);
		TSharedRef<FJsonObject> ValidateOut = MakeShared<FJsonObject>();
		FString ValidateSummary, ValidateErr;
		const bool bValidateOk = Registry.ExecuteTool(TEXT("pcg_graph_validate"), ValidateArgs, ValidateOut, ValidateSummary, ValidateErr);
		OutStructured->SetObjectField(TEXT("post_validate"), ValidateOut);
		OutStructured->SetBoolField(TEXT("post_validate_passed"), bValidateOk);
		if (!bValidateOk)
		{
			TSharedRef<FJsonObject> W = MakeShared<FJsonObject>();
			W->SetStringField(TEXT("kind"), TEXT("post_validate_failed"));
			W->SetStringField(TEXT("detail"), ValidateErr.IsEmpty() ? ValidateSummary : ValidateErr);
			Warnings.Add(MakeShared<FJsonValueObject>(W));
			OutStructured->SetArrayField(TEXT("warnings"), Warnings);
			OutStructured->SetNumberField(TEXT("post_validate_warning_count"), 1);
		}
	}

	OutSummary = FString::Printf(TEXT("Applied template '%s' to '%s': %d/%d nodes, %d/%d edges, %d warnings"),
		*TemplateId, *TargetGraphPath,
		NodesCreated, Spec->Nodes.Num(),
		ConnectionsCreated, Spec->Edges.Num(),
		Warnings.Num());
	if (!bComplete)
	{
		SololmcpError::Set(OutStructured, TEXT("OPERATION_INCOMPLETE"), TEXT("template"),
			TEXT("Template application did not complete all requested graph mutations; inspect warnings before generating."));
		OutError = FString::Printf(
			TEXT("Template '%s' incomplete: %d/%d nodes, %d/%d edges, property_failures=%d, connection_failures=%d"),
			*TemplateId, NodesCreated, Spec->Nodes.Num(), ConnectionsCreated, Spec->Edges.Num(),
			PropertyFailures, ConnectionFailures);
		return false;
	}
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// A10: pcg_graph_snapshot / pcg_graph_restore
// Snapshot = DuplicateAsset(graph, "/Game/PCG/__Snapshots/<tag>").
// Restore  = DeleteAsset(target) + DuplicateAsset(snapshot, target).
// (Users should detach PCG Volumes before restore to avoid dangling references.)
// ═══════════════════════════════════════════════════════════════════════════════
static const TCHAR* SnapshotRoot = TEXT("/Game/PCG/__Snapshots");

static FString MakeSnapshotPath(const FString& OriginalPath, const FString& Tag)
{
	// Extract final component from OriginalPath (e.g. "/Game/PCG/PG_Forest" → "PG_Forest")
	FString Name;
	{
		int32 Slash = INDEX_NONE;
		if (OriginalPath.FindLastChar(TEXT('/'), Slash) && Slash + 1 < OriginalPath.Len())
		{
			Name = OriginalPath.Mid(Slash + 1);
		}
		else
		{
			Name = OriginalPath;
		}
		// Strip any .AssetName suffix.
		int32 Dot = INDEX_NONE;
		if (Name.FindChar(TEXT('.'), Dot)) Name = Name.Left(Dot);
	}
	const FString SafeTag = Tag.IsEmpty()
		? FString::Printf(TEXT("%lld"), static_cast<int64>(FDateTime::UtcNow().ToUnixTimestamp()))
		: Tag;
	return FString::Printf(TEXT("%s/%s__%s"), SnapshotRoot, *Name, *SafeTag);
}

static bool Tool_PcgGraphSnapshot(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}
	FString Tag;
	Arguments->TryGetStringField(TEXT("tag"), Tag);

	// Sanity: confirm source is actually a PCG graph (fail early with a nice message).
	FString LoadErr;
	if (!LoadPCGGraph(Context.Services, AssetPath, LoadErr))
	{
		OutError = FString::Printf(TEXT("Source is not a PCG graph: %s"), *LoadErr);
		return false;
	}

	const FString SnapshotPath = MakeSnapshotPath(AssetPath, Tag);
	FString DupErr;
	UObject* Created = Context.Services.DuplicateAsset(AssetPath, SnapshotPath, DupErr);
	if (!Created)
	{
		OutError = FString::Printf(TEXT("Snapshot failed (%s -> %s): %s"),
			*AssetPath, *SnapshotPath, *DupErr);
		return false;
	}
	FString VerifyErr;
	if (!LoadPCGGraph(Context.Services, SnapshotPath, VerifyErr))
	{
		OutError = FString::Printf(TEXT("Snapshot verification failed after duplicate (%s): %s"),
			*SnapshotPath, *VerifyErr);
		return false;
	}

	OutStructured->SetStringField(TEXT("source_asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("snapshot_asset_path"), SnapshotPath);
	OutStructured->SetStringField(TEXT("tag"), Tag);
	OutStructured->SetNumberField(TEXT("taken_at_unix"), static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp()));
	OutSummary = FString::Printf(TEXT("Snapshot '%s' → '%s'"), *AssetPath, *SnapshotPath);
	return true;
}

static bool Tool_PcgGraphRestore(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString SnapshotPath;
	if (!Arguments->TryGetStringField(TEXT("snapshot_asset_path"), SnapshotPath) || SnapshotPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: snapshot_asset_path");
		return false;
	}
	FString TargetPath;
	if (!Arguments->TryGetStringField(TEXT("target_asset_path"), TargetPath) || TargetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: target_asset_path");
		return false;
	}
	bool bForce = false;
	Arguments->TryGetBoolField(TEXT("force"), bForce);

	// Audit round 7: pcg_graph_restore was failing with "Snapshot is not a PCG graph: Asset is
	// not a UPCGGraph or UPCGGraphInterface with a graph" for snapshots produced by our own
	// pcg_graph_snapshot (e.g. "/Game/PCG/__Snapshots/PG_PineForest_E2E__lc_baseline"). Root
	// cause: MakeSnapshotPath emits a package-only path with no .AssetName suffix, and the
	// editor's FindObject fast-path then resolves it to the UPackage rather than the inner
	// UPCGGraph, failing the cast. The LoadPCGGraph helper above now retries with the
	// canonical "<path>.<leaf>" form and unwraps UPackage to find the inner graph asset, so
	// snapshot paths in either form ("/Game/PCG/Snap/X" or "/Game/PCG/Snap/X.X") now restore.
	FString LoadErr;
	if (!LoadPCGGraph(Context.Services, SnapshotPath, LoadErr))
	{
		OutError = FString::Printf(TEXT("Snapshot is not a PCG graph: %s"), *LoadErr);
		return false;
	}

	// If target exists, delete it (unless bForce=false AND target isn't in the snapshots folder).
	FString ProbeErr;
	UObject* ExistingTarget = Context.Services.LoadAsset(TargetPath, ProbeErr);
	if (ExistingTarget)
	{
		if (!bForce)
		{
			OutError = FString::Printf(TEXT("Target '%s' exists. Pass force=true to overwrite. (Detach PCG Volumes first.)"), *TargetPath);
			return false;
		}
		FString DelErr;
		if (!Context.Services.DeleteAsset(TargetPath, DelErr))
		{
			OutError = FString::Printf(TEXT("Failed to delete existing target '%s': %s"), *TargetPath, *DelErr);
			return false;
		}
	}

	FString DupErr;
	UObject* Restored = Context.Services.DuplicateAsset(SnapshotPath, TargetPath, DupErr);
	if (!Restored)
	{
		OutError = FString::Printf(TEXT("Restore duplicate failed (%s -> %s): %s"),
			*SnapshotPath, *TargetPath, *DupErr);
		return false;
	}
	FString VerifyErr;
	if (!LoadPCGGraph(Context.Services, TargetPath, VerifyErr))
	{
		OutError = FString::Printf(TEXT("Restore verification failed after duplicate (%s): %s"),
			*TargetPath, *VerifyErr);
		return false;
	}

	OutStructured->SetStringField(TEXT("snapshot_asset_path"), SnapshotPath);
	OutStructured->SetStringField(TEXT("target_asset_path"), TargetPath);
	OutStructured->SetBoolField(TEXT("overwrote_existing"), ExistingTarget != nullptr);
	OutStructured->SetNumberField(TEXT("restored_at_unix"), static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp()));
	OutSummary = FString::Printf(TEXT("Restore %s → %s (overwrote=%s)"),
		*SnapshotPath, *TargetPath, ExistingTarget ? TEXT("yes") : TEXT("no"));
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// A11: pcg_generation_budget_set / pcg_generation_budget_get
// In-memory per-graph point budget. Enforcement is advisory: dry_run attaches
// `budget_status` to its response so callers can gate.
// ═══════════════════════════════════════════════════════════════════════════════
static TMap<FString, int64>& BudgetStore()
{
	static TMap<FString, int64> S;
	return S;
}

static bool Tool_PcgBudgetSet(
	const FSololmcpToolExecutionContext& /*Context*/,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}
	double MaxPointsD = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("max_points"), MaxPointsD) || MaxPointsD < 0.0)
	{
		OutError = TEXT("Missing or invalid max_points (must be a non-negative integer)");
		return false;
	}
	const int64 MaxPoints = static_cast<int64>(MaxPointsD);
	// v3.10.x worker-safety: serialize all reads/writes against pcg_generation_budget_get
	// and AttachBudgetStatus (pcg_dry_run) which may run on a TaskGraph worker.
	FScopeLock Lock(&UE::SOMOLMCP::Locks::PcgBudgetStoreLock());
	if (MaxPoints == 0)
	{
		BudgetStore().Remove(AssetPath);
		OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
		OutStructured->SetBoolField(TEXT("cleared"), true);
		OutSummary = FString::Printf(TEXT("Budget cleared for '%s'"), *AssetPath);
		return true;
	}
	BudgetStore().Add(AssetPath, MaxPoints);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetNumberField(TEXT("max_points"), static_cast<double>(MaxPoints));
	OutStructured->SetBoolField(TEXT("cleared"), false);
	OutSummary = FString::Printf(TEXT("Budget set '%s' = %lld pts"), *AssetPath, MaxPoints);
	return true;
}

static bool Tool_PcgBudgetGet(
	const FSololmcpToolExecutionContext& /*Context*/,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		// v3.10.x worker-safety: BudgetStore is shared with pcg_generation_budget_set on the GameThread.
		FScopeLock Lock(&UE::SOMOLMCP::Locks::PcgBudgetStoreLock());
		// No path → return the entire store.
		TSharedRef<FJsonObject> Store = MakeShared<FJsonObject>();
		for (const TPair<FString, int64>& P : BudgetStore())
		{
			Store->SetNumberField(P.Key, static_cast<double>(P.Value));
		}
		OutStructured->SetObjectField(TEXT("budgets"), Store);
		OutStructured->SetNumberField(TEXT("count"), BudgetStore().Num());
		OutSummary = FString::Printf(TEXT("%d budget entries"), BudgetStore().Num());
		return true;
	}
	// v3.10.x worker-safety: BudgetStore is shared with pcg_generation_budget_set on the GameThread.
	FScopeLock Lock(&UE::SOMOLMCP::Locks::PcgBudgetStoreLock());
	const int64* Found = BudgetStore().Find(AssetPath);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetBoolField(TEXT("has_budget"), Found != nullptr);
	if (Found)
	{
		OutStructured->SetNumberField(TEXT("max_points"), static_cast<double>(*Found));
		OutSummary = FString::Printf(TEXT("Budget '%s' = %lld pts"), *AssetPath, *Found);
	}
	else
	{
		OutSummary = FString::Printf(TEXT("No budget set for '%s'"), *AssetPath);
	}
	return true;
}

// Helper for dry_run to attach budget status.
static void AttachBudgetStatus(TSharedRef<FJsonObject>& OutStructured, const FString& AssetPath, double EstimatedPoints)
{
	// v3.10.x worker-safety: same lock as Tool_PcgBudgetSet/Get.
	FScopeLock Lock(&UE::SOMOLMCP::Locks::PcgBudgetStoreLock());
	const int64* Found = BudgetStore().Find(AssetPath);
	if (!Found) return;
	TSharedRef<FJsonObject> BudgetStatus = MakeShared<FJsonObject>();
	BudgetStatus->SetNumberField(TEXT("max_points"), static_cast<double>(*Found));
	BudgetStatus->SetNumberField(TEXT("estimated_points"), EstimatedPoints);
	const bool bOver = EstimatedPoints > static_cast<double>(*Found);
	BudgetStatus->SetBoolField(TEXT("over_budget"), bOver);
	BudgetStatus->SetNumberField(TEXT("headroom"), static_cast<double>(*Found) - EstimatedPoints);
	OutStructured->SetObjectField(TEXT("budget_status"), BudgetStatus);
}

// ═══════════════════════════════════════════════════════════════════════════════
// A5: pcg_generate_async — non-blocking generate via the shared jobs framework.
// A5: pcg_job_poll       — thin PCG-flavoured wrapper around GetJob / AwaitJob.
//
// Rationale: UE editor's pcg_generate can block 5-8 minutes on dense graphs.
// Today every generate call ties up the Rust client's tools_call RPC for that
// duration → UI feels frozen, other agents can't issue any MCP calls.
//
// The plugin already has FSololmcpJobService (frame-friendly step executor),
	// so pcg_generate_async submits pcg_generate plus a component-info check,
	// returning {job_id, status} immediately. pcg_job_poll is a PCG-friendly
// projection of GetJob so the AI doesn't have to know about generic jobs_get.
// ═══════════════════════════════════════════════════════════════════════════════

namespace
{
	static constexpr int32 GPcgAsyncTileFilterMaxTiles = 4;
	static const TCHAR* GPcgAsyncTileFilterContractVersion = TEXT("pcg_async_tile_filter_v2");

	static void AddUniqueString(TArray<FString>& Values, const FString& Value)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return;
		}
		for (const FString& Existing : Values)
		{
			if (Existing.Equals(Trimmed, ESearchCase::IgnoreCase))
			{
				return;
			}
		}
		Values.Add(Trimmed);
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	static bool TryJsonNumberAsInt(const TSharedPtr<FJsonValue>& Value, int32& Out)
	{
		if (!Value.IsValid() || Value->Type != EJson::Number)
		{
			return false;
		}
		const double D = Value->AsNumber();
		const int32 I = FMath::RoundToInt(D);
		if (!FMath::IsNearlyEqual(D, static_cast<double>(I), KINDA_SMALL_NUMBER))
		{
			return false;
		}
		Out = I;
		return true;
	}

	static bool TryJsonObjectIntField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, int32& Out)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		double D = 0.0;
		if (!Obj->TryGetNumberField(FieldName, D))
		{
			return false;
		}
		const int32 I = FMath::RoundToInt(D);
		if (!FMath::IsNearlyEqual(D, static_cast<double>(I), KINDA_SMALL_NUMBER))
		{
			return false;
		}
		Out = I;
		return true;
	}

	static void AddCoordTileHints(int32 Col, int32 Row, TArray<FString>& Keys, TArray<FString>& ActorFragments)
	{
		AddUniqueString(Keys, FString::Printf(TEXT("coord:%d,%d"), Col, Row));
		AddUniqueString(ActorFragments, FString::Printf(TEXT("PCGV_Fill_%d_%d"), Col, Row));
		AddUniqueString(ActorFragments, FString::Printf(TEXT("%d_%d"), Col, Row));
	}

	static bool CollectTileDescriptor(
		const TSharedPtr<FJsonValue>& Value,
		TArray<FString>& Keys,
		TArray<FString>& ActorFragments)
	{
		if (!Value.IsValid() || Value->Type == EJson::Null || Value->Type == EJson::Boolean)
		{
			return false;
		}

		if (Value->Type == EJson::Number)
		{
			int32 Index = 0;
			if (!TryJsonNumberAsInt(Value, Index))
			{
				return false;
			}
			AddUniqueString(Keys, FString::Printf(TEXT("index:%d"), Index));
			AddUniqueString(ActorFragments, FString::Printf(TEXT("%d"), Index));
			return true;
		}

		if (Value->Type == EJson::String)
		{
			const FString Id = Value->AsString().TrimStartAndEnd();
			if (Id.IsEmpty())
			{
				return false;
			}
			AddUniqueString(Keys, FString::Printf(TEXT("id:%s"), *Id));
			AddUniqueString(ActorFragments, Id);
			return true;
		}

		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
			if (Arr.Num() < 2)
			{
				return false;
			}
			int32 Col = 0;
			int32 Row = 0;
			if (!TryJsonNumberAsInt(Arr[0], Col) || !TryJsonNumberAsInt(Arr[1], Row))
			{
				return false;
			}
			AddCoordTileHints(Col, Row, Keys, ActorFragments);
			return true;
		}

		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				return false;
			}

			FString ActorHint;
			if (Obj->TryGetStringField(TEXT("actor"), ActorHint) ||
				Obj->TryGetStringField(TEXT("actor_name"), ActorHint) ||
				Obj->TryGetStringField(TEXT("actor_path"), ActorHint) ||
				Obj->TryGetStringField(TEXT("volume_actor"), ActorHint))
			{
				AddUniqueString(Keys, FString::Printf(TEXT("actor:%s"), *ActorHint));
				AddUniqueString(ActorFragments, ActorHint);
			}

			int32 Col = 0;
			int32 Row = 0;
			if (TryJsonObjectIntField(Obj, TEXT("col"), Col) && TryJsonObjectIntField(Obj, TEXT("row"), Row))
			{
				AddCoordTileHints(Col, Row, Keys, ActorFragments);
				return true;
			}

			int32 Index = 0;
			if (TryJsonObjectIntField(Obj, TEXT("index"), Index) || TryJsonObjectIntField(Obj, TEXT("tile_index"), Index))
			{
				AddUniqueString(Keys, FString::Printf(TEXT("index:%d"), Index));
				AddUniqueString(ActorFragments, FString::Printf(TEXT("%d"), Index));
				return true;
			}

			FString Id;
			if (Obj->TryGetStringField(TEXT("id"), Id) || Obj->TryGetStringField(TEXT("tile_id"), Id))
			{
				AddUniqueString(Keys, FString::Printf(TEXT("id:%s"), *Id));
				AddUniqueString(ActorFragments, Id);
				return true;
			}

			// Unknown object descriptors are accepted as opaque tile receipts. They
			// still count against the hard limit but cannot be identity-checked.
			return true;
		}

		return false;
	}

	static bool ReadTileDescriptorArray(
		const TSharedPtr<FJsonValue>& Field,
		const FString& FieldName,
		int32& OutCount,
		TArray<FString>& OutKeys,
		TArray<FString>& OutActorFragments,
		FString& OutError)
	{
		OutCount = 0;
		if (!Field.IsValid() || Field->IsNull())
		{
			return true;
		}
		if (Field->Type != EJson::Array)
		{
			OutError = FString::Printf(TEXT("%s must be an array when supplied."), *FieldName);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>& Arr = Field->AsArray();
		OutCount = Arr.Num();
		for (int32 I = 0; I < Arr.Num(); ++I)
		{
			if (!CollectTileDescriptor(Arr[I], OutKeys, OutActorFragments))
			{
				OutError = FString::Printf(TEXT("%s[%d] is not a supported tile descriptor."), *FieldName, I);
				return false;
			}
		}
		return true;
	}

	struct FPcgAsyncTileFilterRequest
	{
		TSharedPtr<FJsonValue> AllowedTiles;
		TSharedPtr<FJsonValue> TileIndices;
		bool bHasAllowedTiles = false;
		bool bHasTileIndices = false;
		bool bTileShapeValid = true;
		bool bHardLimitPassed = true;
		bool bGuarded = false;
		bool bEnforced = false;
		int32 AllowedTileCount = 0;
		int32 TileIndexCount = 0;
		int32 RequestedTileCount = 0;
		FString Status = TEXT("not_requested");
		FString EnforcementMode = TEXT("not_requested");
		FString GuardReason;
		FString WhitelistStatus = TEXT("not_applicable");
		TArray<FString> AllowedTileKeys;
		TArray<FString> TileIndexKeys;
		TArray<FString> ActorFragments;

		bool IsRequested() const
		{
			return bHasAllowedTiles || bHasTileIndices;
		}
	};

	static FPcgAsyncTileFilterRequest ReadPcgAsyncTileFilterRequest(const TSharedRef<FJsonObject>& Arguments)
	{
		FPcgAsyncTileFilterRequest Request;
		Request.AllowedTiles = Arguments->TryGetField(TEXT("allowed_tiles"));
		Request.TileIndices = Arguments->TryGetField(TEXT("tile_indices"));
		Request.bHasAllowedTiles = Request.AllowedTiles.IsValid() && !Request.AllowedTiles->IsNull();
		Request.bHasTileIndices = Request.TileIndices.IsValid() && !Request.TileIndices->IsNull();

		if (!Arguments->TryGetStringField(TEXT("tile_filter_status"), Request.Status))
		{
			Request.Status = Request.IsRequested()
				? TEXT("accepted_not_enforced")
				: TEXT("not_requested");
		}
		if (!Arguments->TryGetStringField(TEXT("tile_filter_enforcement_mode"), Request.EnforcementMode))
		{
			Request.EnforcementMode = Request.IsRequested()
				? TEXT("accepted_not_enforced")
				: TEXT("not_requested");
		}
		Arguments->TryGetBoolField(TEXT("tile_filter_guarded"), Request.bGuarded);
		Arguments->TryGetBoolField(TEXT("tile_filter_enforced"), Request.bEnforced);
		Arguments->TryGetStringField(TEXT("tile_filter_guard_reason"), Request.GuardReason);
		Arguments->TryGetStringField(TEXT("tile_filter_whitelist_status"), Request.WhitelistStatus);

		FString ParseError;
		Request.bTileShapeValid =
			ReadTileDescriptorArray(Request.AllowedTiles, TEXT("allowed_tiles"), Request.AllowedTileCount, Request.AllowedTileKeys, Request.ActorFragments, ParseError) &&
			ReadTileDescriptorArray(Request.TileIndices, TEXT("tile_indices"), Request.TileIndexCount, Request.TileIndexKeys, Request.ActorFragments, ParseError);
		if (!Request.bTileShapeValid)
		{
			Request.GuardReason = ParseError;
			Request.Status = TEXT("blocked_malformed_tile_filter");
			Request.EnforcementMode = TEXT("blocked");
		}
		Request.RequestedTileCount = Request.bHasTileIndices ? Request.TileIndexCount : Request.AllowedTileCount;
		Request.bHardLimitPassed = !Request.IsRequested() ||
			(Request.RequestedTileCount > 0 && Request.RequestedTileCount <= GPcgAsyncTileFilterMaxTiles);
		return Request;
	}

	static bool IsSingleActorScope(
		const PcgExecutionSafety::FGenerateTargetSet& Targets,
		FString& OutActorPath,
		FString& OutActorLabel,
		FString& OutActorName)
	{
		if (Targets.Components.Num() == 0)
		{
			return false;
		}

		OutActorPath = Targets.Components[0].ActorPath;
		OutActorLabel = Targets.Components[0].ActorLabel;
		OutActorName = Targets.Components[0].ActorName;
		if (OutActorPath.IsEmpty())
		{
			return false;
		}

		for (const PcgExecutionSafety::FGenerateComponentTarget& Target : Targets.Components)
		{
			if (!Target.ActorPath.Equals(OutActorPath, ESearchCase::IgnoreCase))
			{
				return false;
			}
		}
		return true;
	}

	static bool TileKeysAreSubset(const TArray<FString>& Needles, const TArray<FString>& Haystack)
	{
		for (const FString& Needle : Needles)
		{
			bool bFound = false;
			for (const FString& Candidate : Haystack)
			{
				if (Needle.Equals(Candidate, ESearchCase::IgnoreCase))
				{
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				return false;
			}
		}
		return true;
	}

	static bool ActorScopeMatchesTileHints(
		const PcgExecutionSafety::FGenerateTargetSet& Targets,
		const FPcgAsyncTileFilterRequest& Request)
	{
		if (Request.ActorFragments.Num() == 0 || Targets.Components.Num() == 0 || !Targets.Components[0].Actor)
		{
			return false;
		}

		FString Haystack = Targets.Components[0].ActorLabel + TEXT(" ") +
			Targets.Components[0].ActorName + TEXT(" ") +
			Targets.Components[0].ActorPath;
		for (const FName& Tag : Targets.Components[0].Actor->Tags)
		{
			Haystack += TEXT(" ");
			Haystack += Tag.ToString();
		}

		for (const FString& Fragment : Request.ActorFragments)
		{
			if (!Fragment.IsEmpty() && Haystack.Contains(Fragment, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static bool EvaluatePcgAsyncTileFilterContract(
		const PcgExecutionSafety::FGenerateTargetSet& Targets,
		FPcgAsyncTileFilterRequest& Request,
		FString& OutError)
	{
		if (!Request.IsRequested())
		{
			Request.Status = TEXT("not_requested");
			Request.EnforcementMode = TEXT("not_requested");
			Request.WhitelistStatus = TEXT("not_applicable");
			return true;
		}

		if (!Request.bTileShapeValid)
		{
			Request.Status = TEXT("blocked_malformed_tile_filter");
			Request.EnforcementMode = TEXT("blocked");
			Request.bHardLimitPassed = false;
			OutError = Request.GuardReason.IsEmpty()
				? TEXT("Malformed tile filter.")
				: Request.GuardReason;
			return false;
		}

		if (Request.RequestedTileCount <= 0)
		{
			Request.Status = TEXT("blocked_empty_tile_filter");
			Request.EnforcementMode = TEXT("blocked");
			Request.bHardLimitPassed = false;
			Request.GuardReason = TEXT("Tile-filtered pcg_generate_async requests must name at least one tile; refusing to submit a no-op filter as a broad generate.");
			OutError = Request.GuardReason;
			return false;
		}

		if (Request.RequestedTileCount > GPcgAsyncTileFilterMaxTiles)
		{
			Request.Status = TEXT("blocked_tile_hard_limit");
			Request.EnforcementMode = TEXT("blocked");
			Request.bHardLimitPassed = false;
			Request.GuardReason = FString::Printf(
				TEXT("Tile-filtered pcg_generate_async requests are capped at %d tile(s); request named %d."),
				GPcgAsyncTileFilterMaxTiles,
				Request.RequestedTileCount);
			OutError = Request.GuardReason;
			return false;
		}
		Request.bHardLimitPassed = true;

		if (Request.bHasAllowedTiles && Request.bHasTileIndices)
		{
			if (Request.AllowedTileKeys.Num() > 0 && Request.TileIndexKeys.Num() > 0)
			{
				if (!TileKeysAreSubset(Request.TileIndexKeys, Request.AllowedTileKeys))
				{
					Request.Status = TEXT("blocked_tile_whitelist_mismatch");
					Request.EnforcementMode = TEXT("blocked");
					Request.WhitelistStatus = TEXT("mismatch");
					Request.GuardReason = TEXT("tile_indices must be a subset of allowed_tiles when both fields are supplied.");
					OutError = Request.GuardReason;
					return false;
				}
				Request.WhitelistStatus = TEXT("subset_verified");
			}
			else
			{
				Request.WhitelistStatus = TEXT("not_comparable");
			}
		}

		FString ActorPath;
		FString ActorLabel;
		FString ActorName;
		if (Targets.bAllowAll || !IsSingleActorScope(Targets, ActorPath, ActorLabel, ActorName))
		{
			Request.Status = TEXT("blocked_requires_single_actor_scope");
			Request.EnforcementMode = TEXT("blocked");
			Request.GuardReason = TEXT("Tile-filtered pcg_generate_async cannot safely delegate to pcg_generate for graph-only, allow_all, or multi-actor targets. Bind each tile to an explicit PCG volume actor and submit one actor-scoped job per tile or per <=4-tile batch.");
			OutError = Request.GuardReason;
			return false;
		}

		Request.bGuarded = true;
		Request.bEnforced = false;
		Request.Status = TEXT("guarded_actor_scope");
		Request.EnforcementMode = ActorScopeMatchesTileHints(Targets, Request)
			? TEXT("actor_scope_verified_not_tile_masked")
			: TEXT("actor_scope_guarded_not_tile_masked");
		Request.GuardReason = TEXT("Native pcg_generate has no tile mask; this job is hard-limited by requested tile count and forced through one resolved actor scope.");
		return true;
	}

	static void AttachPcgAsyncTileFilterFields(
		const TSharedRef<FJsonObject>& Out,
		const FPcgAsyncTileFilterRequest& Request)
	{
		if (Request.bHasAllowedTiles)
		{
			Out->SetField(TEXT("allowed_tiles"), Request.AllowedTiles);
		}
		if (Request.bHasTileIndices)
		{
			Out->SetField(TEXT("tile_indices"), Request.TileIndices);
		}
		Out->SetStringField(TEXT("tile_filter_contract_version"), GPcgAsyncTileFilterContractVersion);
		Out->SetBoolField(TEXT("tile_filter_requested"), Request.IsRequested());
		Out->SetStringField(TEXT("tile_filter_status"), Request.Status);
		Out->SetStringField(TEXT("tile_filter_enforcement_mode"), Request.EnforcementMode);
		Out->SetBoolField(TEXT("tile_filter_enforced"), Request.bEnforced);
		Out->SetBoolField(TEXT("tile_filter_guarded"), Request.bGuarded);
		Out->SetBoolField(TEXT("tile_filter_hard_limit_passed"), Request.bHardLimitPassed);
		Out->SetNumberField(TEXT("tile_filter_max_tiles_per_request"), GPcgAsyncTileFilterMaxTiles);
		Out->SetNumberField(TEXT("tile_filter_requested_tile_count"), Request.RequestedTileCount);
		Out->SetNumberField(TEXT("tile_filter_allowed_tile_count"), Request.AllowedTileCount);
		Out->SetNumberField(TEXT("tile_filter_tile_index_count"), Request.TileIndexCount);
		Out->SetStringField(TEXT("tile_filter_whitelist_status"), Request.WhitelistStatus);
		Out->SetStringField(TEXT("tile_cap_schema"), PcgExecutionSafety::PcgTileCapSchema);
		Out->SetStringField(
			TEXT("tile_cap_status"),
			Request.Status.StartsWith(TEXT("blocked")) ? TEXT("block") : (Request.IsRequested() ? TEXT("pass") : TEXT("warn")));
		Out->SetStringField(
			TEXT("tile_cap_observed_source"),
			Request.bHasTileIndices ? TEXT("tile_indices") : (Request.bHasAllowedTiles ? TEXT("allowed_tiles") : TEXT("none")));
		Out->SetStringField(
			TEXT("tile_cap_guard_reason"),
			Request.GuardReason.IsEmpty() ? TEXT("pcg_generate_async tile-cap metadata attached for receipt review.") : Request.GuardReason);
		Out->SetBoolField(TEXT("tile_cap_fail_closed"), Request.Status.StartsWith(TEXT("blocked")));
		Out->SetBoolField(TEXT("tile_batch_count_known"), Request.IsRequested());
		Out->SetNumberField(TEXT("tile_batch_count"), Request.RequestedTileCount);
		Out->SetBoolField(TEXT("tile_mask_native_enforced"), false);
		Out->SetBoolField(TEXT("tile_mask_receipt_only"), Request.IsRequested());
		Out->SetStringField(
			TEXT("tile_mask_status"),
			Request.IsRequested() ? TEXT("receipt_evidence_only_actor_scope_generate") : TEXT("not_requested"));
		Out->SetStringField(
			TEXT("tile_mask_note"),
			TEXT("pcg_generate_async forwards to pcg_generate; current UE path has no native tile mask, so <=4 tile evidence is enforced before actor-scoped submission."));
		TSharedRef<FJsonObject> TileCapPolicy = MakeShared<FJsonObject>();
		TileCapPolicy->SetStringField(TEXT("schema"), PcgExecutionSafety::PcgTileCapSchema);
		TileCapPolicy->SetNumberField(TEXT("max_tiles_per_generate"), GPcgAsyncTileFilterMaxTiles);
		TileCapPolicy->SetBoolField(TEXT("require_tile_evidence"), Request.IsRequested());
		TileCapPolicy->SetBoolField(TEXT("fail_closed"), Request.Status.StartsWith(TEXT("blocked")));
		TileCapPolicy->SetStringField(TEXT("enforcement_mode"), Request.EnforcementMode);
		TileCapPolicy->SetBoolField(TEXT("native_tile_mask_available"), false);
		Out->SetObjectField(TEXT("tile_cap_policy"), TileCapPolicy);
		if (!Request.GuardReason.IsEmpty())
		{
			Out->SetStringField(TEXT("tile_filter_guard_reason"), Request.GuardReason);
		}
		if (Request.AllowedTileKeys.Num() > 0)
		{
			Out->SetArrayField(TEXT("tile_filter_allowed_tile_keys"), StringArrayToJson(Request.AllowedTileKeys));
		}
		if (Request.TileIndexKeys.Num() > 0)
		{
			Out->SetArrayField(TEXT("tile_filter_tile_index_keys"), StringArrayToJson(Request.TileIndexKeys));
		}
		if (Request.ActorFragments.Num() > 0)
		{
			Out->SetArrayField(TEXT("tile_filter_actor_identity_hints"), StringArrayToJson(Request.ActorFragments));
		}
	}

	static PcgExecutionSafety::FTileCapDecision BuildTileCapDecisionFromAsyncFilter(
		const FPcgAsyncTileFilterRequest& Request)
	{
		PcgExecutionSafety::FTileCapDecision Decision;
		Decision.ToolName = TEXT("pcg_generate_async");
		Decision.Status = Request.Status.StartsWith(TEXT("blocked")) ? TEXT("block") : (Request.IsRequested() ? TEXT("pass") : TEXT("warn"));
		Decision.ObservedSource = Request.bHasTileIndices ? TEXT("tile_indices") : (Request.bHasAllowedTiles ? TEXT("allowed_tiles") : TEXT("none"));
		Decision.Reason = Request.GuardReason;
		Decision.ObservedTileCount = Request.IsRequested() ? Request.RequestedTileCount : -1;
		Decision.bStrictOrUnattended = true;
		Decision.bRequireTileEvidence = Request.IsRequested();
		Decision.bFailClosed = Decision.Status == TEXT("block");
		return Decision;
	}

	static void AddPcgAsyncTileFilterWarning(
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		const FPcgAsyncTileFilterRequest& Request)
	{
		if (!Request.IsRequested())
		{
			return;
		}

		if (Request.Status == TEXT("guarded_actor_scope"))
		{
			PcgExecutionSafety::AddWarning(
				Warnings,
				TEXT("tile_filter_actor_scope_only"),
				TEXT("pcg_generate_async cannot apply a native PCG tile mask; it is guarded by <=4 requested tiles and a single resolved actor scope."),
				TEXT("For unattended large worlds, keep using one explicit per-tile PCG volume actor per async job and verify tile_filter_enforcement_mode in the receipt."));
			return;
		}

		if (Request.Status == TEXT("accepted_not_enforced"))
		{
			PcgExecutionSafety::AddWarning(
				Warnings,
				TEXT("tile_filter_accepted_not_enforced"),
				TEXT("pcg_generate_async accepted allowed_tiles/tile_indices for receipt compatibility, but the current executor still delegates to pcg_generate without tile-level enforcement."),
				TEXT("Use pcg_incremental_fill or explicit per-tile actor/volume targeting for hard tile limits until executor enforcement lands."));
		}
	}

	static bool AttachPcgAsyncTileFilterFromStepResults(
		const TArray<TSharedPtr<FJsonValue>>* StepResults,
		const TSharedRef<FJsonObject>& Out)
	{
		if (!StepResults)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& V : *StepResults)
		{
			const TSharedPtr<FJsonObject> StepObj = V.IsValid() ? V->AsObject() : nullptr;
			if (!StepObj.IsValid())
			{
				continue;
			}

			FString Tool;
			if (!StepObj->TryGetStringField(TEXT("tool"), Tool) || Tool != TEXT("pcg_generate"))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
			if (!StepObj->TryGetObjectField(TEXT("arguments"), ArgsPtr) || !ArgsPtr || !ArgsPtr->IsValid())
			{
				continue;
			}

			const FPcgAsyncTileFilterRequest Request = ReadPcgAsyncTileFilterRequest(ArgsPtr->ToSharedRef());
			AttachPcgAsyncTileFilterFields(Out, Request);
			if (Request.IsRequested())
			{
				TArray<TSharedPtr<FJsonValue>> Warnings;
				const TArray<TSharedPtr<FJsonValue>>* ExistingWarnings = nullptr;
				if (Out->TryGetArrayField(TEXT("warnings"), ExistingWarnings) && ExistingWarnings)
				{
					Warnings = *ExistingWarnings;
				}
				AddPcgAsyncTileFilterWarning(Warnings, Request);
				Out->SetArrayField(TEXT("warnings"), Warnings);
			}
			return true;
		}
		return false;
	}
}

static bool Tool_PcgGenerateAsync_Impl(
	FSololmcpToolRegistry& Registry,
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	PcgExecutionSafety::FGenerateTargetSet Targets;
	if (!PcgExecutionSafety::ResolveGenerateTargets(Context.Services, Arguments, Targets, OutError))
	{
		return false;
	}
	FPcgAsyncTileFilterRequest TileFilter = ReadPcgAsyncTileFilterRequest(Arguments);
	FString TileFilterError;
	if (!EvaluatePcgAsyncTileFilterContract(Targets, TileFilter, TileFilterError))
	{
		PcgExecutionSafety::AttachResolutionFields(OutStructured, Targets);
		AttachPcgAsyncTileFilterFields(OutStructured, TileFilter);
		PcgExecutionSafety::AttachGenerateReceiptEnvelope(
			OutStructured,
			Arguments,
			Targets,
			BuildTileCapDecisionFromAsyncFilter(TileFilter),
			-1,
			0);
		SololmcpError::Set(OutStructured, TEXT("VALIDATION_FAILED"), TEXT("allowed_tiles"), TileFilterError);
		OutError = TileFilterError;
		return false;
	}
	if (TileFilter.IsRequested())
	{
		AddPcgAsyncTileFilterWarning(Targets.Warnings, TileFilter);
	}
	PcgExecutionSafety::AttachResolutionFields(OutStructured, Targets);
	PcgExecutionSafety::AttachPcgEditorSubwindowCleanupEvidence(OutStructured, TEXT("pcg_generate_async"));
	AttachPcgAsyncTileFilterFields(OutStructured, TileFilter);

	TArray<TSharedPtr<FJsonValue>> ValidationReports;
	if (!PcgExecutionSafety::ValidateGraphPathsForGenerate(Registry, Targets.UniqueGraphPaths, ValidationReports, OutError))
	{
		OutStructured->SetArrayField(TEXT("validation"), ValidationReports);
		return false;
	}
	OutStructured->SetArrayField(TEXT("validation"), ValidationReports);

	bool bSingleActorTarget = Targets.Components.Num() > 0;
	for (const PcgExecutionSafety::FGenerateComponentTarget& Target : Targets.Components)
	{
		if (Target.ActorPath != Targets.Components[0].ActorPath)
		{
			bSingleActorTarget = false;
			break;
		}
	}
	const FString ResolvedActorPath = bSingleActorTarget ? Targets.Components[0].ActorPath : FString();
	const FString ResolvedActorLabel = bSingleActorTarget ? Targets.Components[0].ActorLabel : FString();

	// Compose the generation step. The legacy pcg_generate implementation reads
	// actor_label, while newer PCG helpers read actor, so pass both when a single
	// actor was safely resolved. Graph fields are forwarded as canonical paths.
	TSharedRef<FJsonObject> StepArgs = MakeShared<FJsonObject>();
	if (bSingleActorTarget)
	{
		StepArgs->SetStringField(TEXT("actor"), ResolvedActorPath);
		StepArgs->SetStringField(TEXT("actor_label"), ResolvedActorLabel);
	}
	if (Targets.UniqueGraphPaths.Num() == 1)
	{
		StepArgs->SetStringField(TEXT("graph_path"), Targets.UniqueGraphPaths[0]);
		StepArgs->SetStringField(TEXT("asset_path"), Targets.UniqueGraphPaths[0]);
	}
	if (Targets.bAllowAll)
	{
		StepArgs->SetBoolField(TEXT("allow_all"), true);
	}
	AttachPcgAsyncTileFilterFields(StepArgs, TileFilter);

	FString ClientRequestId;
	if (Arguments->TryGetStringField(TEXT("client_request_id"), ClientRequestId) && !ClientRequestId.IsEmpty())
	{
		StepArgs->SetStringField(TEXT("client_request_id"), ClientRequestId);
	}
	FString TraceId;
	if (Arguments->TryGetStringField(TEXT("trace_id"), TraceId) && !TraceId.IsEmpty())
	{
		StepArgs->SetStringField(TEXT("trace_id"), TraceId);
	}

	TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("tool"), TEXT("pcg_generate"));
	Step->SetObjectField(TEXT("arguments"), StepArgs);
	const FString StepLabel = bSingleActorTarget
		? FString::Printf(TEXT("pcg_generate(%s)"), *ResolvedActorLabel)
		: FString::Printf(TEXT("pcg_generate(%d targets)"), Targets.Components.Num());
	Step->SetStringField(TEXT("label"), StepLabel);

	TArray<TSharedPtr<FJsonValue>> StepsArray;
	StepsArray.Add(MakeShared<FJsonValueObject>(Step));
	if (bSingleActorTarget)
	{
		TSharedRef<FJsonObject> VerifyArgs = MakeShared<FJsonObject>();
		VerifyArgs->SetStringField(TEXT("actor"), ResolvedActorPath);
		TSharedRef<FJsonObject> VerifyStep = MakeShared<FJsonObject>();
		VerifyStep->SetStringField(TEXT("tool"), TEXT("pcg_component_info"));
		VerifyStep->SetObjectField(TEXT("arguments"), VerifyArgs);
		VerifyStep->SetStringField(TEXT("label"), FString::Printf(TEXT("pcg_component_info(%s)"), *ResolvedActorLabel));
		StepsArray.Add(MakeShared<FJsonValueObject>(VerifyStep));
	}
	else
	{
		PcgExecutionSafety::AddWarning(
			Targets.Warnings,
			TEXT("async_verify_skipped"),
			TEXT("pcg_component_info verification is skipped because the request resolves to multiple actors."));
		OutStructured->SetArrayField(TEXT("warnings"), Targets.Warnings);
	}

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("steps"), StepsArray);
	Params->SetStringField(TEXT("plan_label"), bSingleActorTarget
		? FString::Printf(TEXT("pcg_generate_async(%s)"), *ResolvedActorLabel)
		: FString::Printf(TEXT("pcg_generate_async(%d targets)"), Targets.Components.Num()));
	if (Targets.UniqueGraphPaths.Num() == 1)
	{
		Params->SetStringField(TEXT("graph_path"), Targets.UniqueGraphPaths[0]);
	}
	Params->SetArrayField(TEXT("graph_paths"), PcgExecutionSafety::GraphPathsToJson(Targets.UniqueGraphPaths));
	AttachPcgAsyncTileFilterFields(Params, TileFilter);

	// Forward optional client_request_id / trace_id for idempotent dedup and observability.
	if (!ClientRequestId.IsEmpty())
	{
		Params->SetStringField(TEXT("client_request_id"), ClientRequestId);
	}
	if (!TraceId.IsEmpty())
	{
		Params->SetStringField(TEXT("trace_id"), TraceId);
	}

	FString SubmitError;
	TSharedRef<FJsonObject> SubmitResult = MakeShared<FJsonObject>();
	if (!FSololmcpJobService::SubmitJob(Params, SubmitResult, SubmitError))
	{
		OutError = FString::Printf(TEXT("jobs/submit failed: %s"), *SubmitError);
		return false;
	}

	// SubmitResult typically contains job_id + status. Copy every field through
	// so the AI sees exactly what jobs/submit returned, plus PCG-specific hints.
	for (const auto& Pair : SubmitResult->Values)
	{
		OutStructured->SetField(FString(*Pair.Key), Pair.Value);
	}

	FString JobId;
	OutStructured->TryGetStringField(TEXT("job_id"), JobId);
	if (bSingleActorTarget)
	{
		OutStructured->SetStringField(TEXT("actor"), ResolvedActorLabel);
		OutStructured->SetStringField(TEXT("actor_path"), ResolvedActorPath);
	}
	if (!ClientRequestId.IsEmpty())
	{
		OutStructured->SetStringField(TEXT("client_request_id"), ClientRequestId);
	}
	if (!TraceId.IsEmpty())
	{
		OutStructured->SetStringField(TEXT("trace_id"), TraceId);
	}
	AttachPcgAsyncTileFilterFields(OutStructured, TileFilter);
	PcgExecutionSafety::AttachGenerateReceiptEnvelope(
		OutStructured,
		Arguments,
		Targets,
		BuildTileCapDecisionFromAsyncFilter(TileFilter),
		-1,
		0);
	OutStructured->SetStringField(TEXT("poll_tool"), TEXT("pcg_job_poll"));
	OutStructured->SetStringField(
		TEXT("next_step"),
		TEXT("Poll pcg_job_poll {job_id} until status='succeeded' or 'failed'. "
		     "Use wait_ms>0 for a short server-side wait (up to 5000 ms) to reduce polling chatter. "
		     "Single-actor jobs include a pcg_component_info verification step after pcg_generate."));

	OutSummary = JobId.IsEmpty()
		? FString::Printf(TEXT("pcg_generate_async submitted for %d component(s)"), Targets.Components.Num())
		: FString::Printf(TEXT("pcg_generate_async submitted for %d component(s) -> job_id=%s"), Targets.Components.Num(), *JobId);
	return true;
}

static bool Tool_PcgJobPoll_Impl(
	FSololmcpToolRegistry& Registry,
	const FSololmcpToolExecutionContext& /*Context*/,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString JobId;
	if (!Arguments->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
	{
		OutError = TEXT("Missing required argument: job_id");
		return false;
	}

	double WaitMsD = 0.0;
	Arguments->TryGetNumberField(TEXT("wait_ms"), WaitMsD);
	// Clamp — we don't want the caller holding the RPC thread for minutes.
	const int32 WaitMs = FMath::Clamp(static_cast<int32>(WaitMsD), 0, 5000);

	FString PollError;
	TSharedRef<FJsonObject> JobState = MakeShared<FJsonObject>();
	bool bOk = false;
	if (WaitMs > 0)
	{
		bOk = FSololmcpJobService::AwaitJob(Registry, JobId, WaitMs, JobState, PollError);
	}
	else
	{
		bOk = FSololmcpJobService::GetJob(JobId, JobState, PollError);
	}

	if (!bOk)
	{
		OutError = FString::Printf(TEXT("jobs/%s failed: %s"),
			WaitMs > 0 ? TEXT("await") : TEXT("get"), *PollError);
		return false;
	}

	// Copy whatever the job service returned (status / started_at / finished_at /
	// error / results / event log tail — shape is owned by JobService).
	for (const auto& Pair : JobState->Values)
	{
		OutStructured->SetField(FString(*Pair.Key), Pair.Value);
	}
	PcgExecutionSafety::AttachPcgEditorSubwindowCleanupEvidence(OutStructured, TEXT("pcg_job_poll"));

	FString Status;
	OutStructured->TryGetStringField(TEXT("status"), Status);
	const FString StatusLower = Status.ToLower();
	double CurrentStepD = 0.0;
	double TotalStepsD = 0.0;
	OutStructured->TryGetNumberField(TEXT("current_step"), CurrentStepD);
	OutStructured->TryGetNumberField(TEXT("total_steps"), TotalStepsD);
	const TArray<TSharedPtr<FJsonValue>>* StepResults = nullptr;
	OutStructured->TryGetArrayField(TEXT("step_results"), StepResults);
	AttachPcgAsyncTileFilterFromStepResults(StepResults, OutStructured);
	bool bAnyStepFailed = false;
	if (StepResults)
	{
		for (const TSharedPtr<FJsonValue>& V : *StepResults)
		{
			const TSharedPtr<FJsonObject> StepObj = V.IsValid() ? V->AsObject() : nullptr;
			if (!StepObj.IsValid()) continue;
			bool bStepOk = true;
			if (StepObj->TryGetBoolField(TEXT("ok"), bStepOk) && !bStepOk)
			{
				bAnyStepFailed = true;
				break;
			}
		}
	}
	const int32 CurrentStep = static_cast<int32>(CurrentStepD);
	const int32 TotalSteps = static_cast<int32>(TotalStepsD);
	const int32 ResultCount = StepResults ? StepResults->Num() : 0;
	const bool bLooksIncomplete = TotalSteps > 0 && (CurrentStep < TotalSteps || ResultCount < TotalSteps);
	if (StatusLower == TEXT("succeeded") && bAnyStepFailed)
	{
		Status = TEXT("failed");
		OutStructured->SetStringField(TEXT("status"), Status);
		OutStructured->SetStringField(TEXT("effective_status"), Status);
		OutStructured->SetStringField(TEXT("error_code"), TEXT("STEP_FAILED"));
		OutStructured->SetStringField(TEXT("error_message"), TEXT("Job service reported succeeded, but at least one step_result has ok=false."));
	}
	else if (StatusLower == TEXT("succeeded") && bLooksIncomplete)
	{
		Status = TEXT("running");
		OutStructured->SetStringField(TEXT("status"), Status);
		OutStructured->SetStringField(TEXT("effective_status"), Status);
		OutStructured->SetStringField(TEXT("completion_note"), TEXT("Job service reported succeeded before all step results were present; treating as still running."));
	}
	else
	{
		OutStructured->SetStringField(TEXT("effective_status"), Status);
	}
	const FString EffectiveStatusLower = Status.ToLower();
	const bool bTerminal = (EffectiveStatusLower == TEXT("succeeded") ||
	                        EffectiveStatusLower == TEXT("failed") ||
	                        EffectiveStatusLower == TEXT("cancelled"));

	OutStructured->SetBoolField(TEXT("terminal"), bTerminal);
	if (!bTerminal)
	{
		OutStructured->SetStringField(
			TEXT("hint"),
			TEXT("Job still running. Call pcg_job_poll again with wait_ms=5000 "
			     "to piggy-back on the next completion event."));
	}
	else if (EffectiveStatusLower == TEXT("succeeded"))
	{
		OutStructured->SetStringField(
			TEXT("hint"),
			TEXT("Generation finished. Consider pcg_graph_explain / actor_list to verify outputs."));
	}
	else if (EffectiveStatusLower == TEXT("failed"))
	{
		OutStructured->SetStringField(
			TEXT("hint"),
			TEXT("Generation failed. Run pcg_graph_validate on the graph and inspect the error field."));
	}

	OutSummary = FString::Printf(TEXT("job %s status=%s%s"),
		*JobId, Status.IsEmpty() ? TEXT("unknown") : *Status,
		bTerminal ? TEXT(" (terminal)") : TEXT(""));
	if (EffectiveStatusLower == TEXT("failed"))
	{
		FString ErrorMessage;
		OutStructured->TryGetStringField(TEXT("error_message"), ErrorMessage);
		OutError = ErrorMessage.IsEmpty()
			? FString::Printf(TEXT("PCG job %s failed."), *JobId)
			: ErrorMessage;
		return false;
	}
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// A4: pcg_partition_preview — multi-tile dry_run projection over an AOI.
//
// Treats every tile as identical (same graph, same density). Useful for the
// pcg-incremental-fill skill's plan phase: in one call you get tiles_total /
// batches_total / per_tile_estimated_points / over_budget_tile_count without
// having to reimplement the math in prompt-land.
//
// Shares the A9 heuristic under the hood. Calls into the same LoadPCGGraph +
// node-categorization logic as pcg_dry_run, just multiplies by tile count.
// ═══════════════════════════════════════════════════════════════════════════════
static bool Tool_PcgPartitionPreview(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	double MinX = 0.0, MinY = 0.0, MaxX = 0.0, MaxY = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("min_x"), MinX) ||
	    !Arguments->TryGetNumberField(TEXT("min_y"), MinY) ||
	    !Arguments->TryGetNumberField(TEXT("max_x"), MaxX) ||
	    !Arguments->TryGetNumberField(TEXT("max_y"), MaxY))
	{
		OutError = TEXT("Missing required AOI bounds: min_x / min_y / max_x / max_y (centimeters by UE convention, OR meters — caller decides; we just multiply)");
		return false;
	}
	if (MaxX <= MinX || MaxY <= MinY)
	{
		OutError = TEXT("AOI bounds invalid: max_x>min_x and max_y>min_y required");
		return false;
	}

	double TileSizeM = 256.0;
	Arguments->TryGetNumberField(TEXT("tile_size_m"), TileSizeM);
	TileSizeM = FMath::Max(1.0, TileSizeM);

	double DensityPerM2 = 0.2;
	Arguments->TryGetNumberField(TEXT("default_density_per_m2"), DensityPerM2);

	double BatchSizeD = 4.0;
	Arguments->TryGetNumberField(TEXT("batch_size"), BatchSizeD);
	const int32 BatchSize = FMath::Clamp(static_cast<int32>(BatchSizeD), 1, 32);

	double BudgetPerTileD = 0.0;
	Arguments->TryGetNumberField(TEXT("per_tile_budget"), BudgetPerTileD);
	const double PerTileBudget = FMath::Max(0.0, BudgetPerTileD);

	double SampleLimitD = 16.0;
	Arguments->TryGetNumberField(TEXT("sample_batches"), SampleLimitD);
	const int32 SampleBatchesCap = FMath::Clamp(static_cast<int32>(SampleLimitD), 0, 1024);

	UPCGGraph* Graph = LoadPCGGraph(Context.Services, AssetPath, OutError);
	if (!Graph) return false;

	// Mirror A9 heuristic so per-tile estimates stay consistent with pcg_dry_run.
	int32 SamplerCount = 0, FilterCount = 0, SpawnCount = 0;
	double SurvivalRate = 1.0;
	for (UPCGNode* Node : Graph->GetNodes())
	{
		if (!Node || !Node->GetSettings()) continue;
		const FString Cat = CategorizeSettings(Node->GetSettings()->GetClass());
		if (Cat == TEXT("sampling") || Cat == TEXT("landscape") || Cat == TEXT("spline")) ++SamplerCount;
		else if (Cat == TEXT("filter") || Cat == TEXT("density")) { ++FilterCount; SurvivalRate *= 0.6; }
		else if (Cat == TEXT("spawn")) ++SpawnCount;
	}

	const double TileAreaM2 = TileSizeM * TileSizeM;
	const double PerTileRawPoints = FMath::Max(1.0, TileAreaM2 * DensityPerM2 * FMath::Max(1, SamplerCount));
	const double PerTileEstimated = PerTileRawPoints * SurvivalRate;

	const double AoiWidthM  = MaxX - MinX;
	const double AoiHeightM = MaxY - MinY;
	const int32 Cols = FMath::Max(1, FMath::CeilToInt(AoiWidthM  / TileSizeM));
	const int32 Rows = FMath::Max(1, FMath::CeilToInt(AoiHeightM / TileSizeM));
	const int32 TilesTotal = Cols * Rows;
	const int32 BatchesTotal = FMath::DivideAndRoundUp(TilesTotal, BatchSize);
	const double EstimatedTotalPoints = PerTileEstimated * TilesTotal;

	const bool bOverBudget = (PerTileBudget > 0.0) && (PerTileEstimated > PerTileBudget);
	const int32 OverBudgetTileCount = bOverBudget ? TilesTotal : 0;

	// Build the output.
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);

	TSharedRef<FJsonObject> AoiOut = MakeShared<FJsonObject>();
	AoiOut->SetNumberField(TEXT("min_x"), MinX);
	AoiOut->SetNumberField(TEXT("min_y"), MinY);
	AoiOut->SetNumberField(TEXT("max_x"), MaxX);
	AoiOut->SetNumberField(TEXT("max_y"), MaxY);
	AoiOut->SetNumberField(TEXT("tile_size_m"), TileSizeM);
	AoiOut->SetNumberField(TEXT("cols"), Cols);
	AoiOut->SetNumberField(TEXT("rows"), Rows);
	OutStructured->SetObjectField(TEXT("aoi"), AoiOut);

	OutStructured->SetNumberField(TEXT("tiles_total"), TilesTotal);
	OutStructured->SetNumberField(TEXT("batch_size"), BatchSize);
	OutStructured->SetNumberField(TEXT("batches_total"), BatchesTotal);
	OutStructured->SetNumberField(TEXT("per_tile_area_m2"), TileAreaM2);
	OutStructured->SetNumberField(TEXT("per_tile_estimated_points"), PerTileEstimated);
	OutStructured->SetNumberField(TEXT("estimated_total_points"), EstimatedTotalPoints);
	OutStructured->SetNumberField(TEXT("per_tile_budget"), PerTileBudget);
	OutStructured->SetBoolField(TEXT("per_tile_over_budget"), bOverBudget);
	OutStructured->SetNumberField(TEXT("over_budget_tile_count"), OverBudgetTileCount);
	OutStructured->SetBoolField(TEXT("is_heuristic"), true);

	// Emit per-batch samples (cap at SampleBatchesCap to keep response small).
	TArray<TSharedPtr<FJsonValue>> BatchSamples;
	const int32 BatchesToEmit = FMath::Min(BatchesTotal, SampleBatchesCap);
	for (int32 B = 0; B < BatchesToEmit; ++B)
	{
		const int32 FirstTile = B * BatchSize;
		const int32 LastTileExclusive = FMath::Min(FirstTile + BatchSize, TilesTotal);
		const int32 TilesInBatch = LastTileExclusive - FirstTile;

		TSharedRef<FJsonObject> BatchObj = MakeShared<FJsonObject>();
		BatchObj->SetNumberField(TEXT("batch_index"), B);
		BatchObj->SetNumberField(TEXT("first_tile"), FirstTile);
		BatchObj->SetNumberField(TEXT("tile_count"), TilesInBatch);
		BatchObj->SetNumberField(TEXT("estimated_points"), PerTileEstimated * TilesInBatch);
		// tile grid coords (row, col) for the first tile — AI can increment from there.
		BatchObj->SetNumberField(TEXT("first_tile_col"), FirstTile % Cols);
		BatchObj->SetNumberField(TEXT("first_tile_row"), FirstTile / Cols);
		BatchSamples.Add(MakeShared<FJsonValueObject>(BatchObj));
	}
	OutStructured->SetArrayField(TEXT("batch_samples"), BatchSamples);
	if (BatchesToEmit < BatchesTotal)
	{
		OutStructured->SetStringField(
			TEXT("batch_samples_note"),
			FString::Printf(TEXT("Emitted first %d of %d batches to cap response size; all batches follow the same per_tile_estimated_points."), BatchesToEmit, BatchesTotal));
	}

	OutStructured->SetStringField(
		TEXT("next_step"),
		bOverBudget
			? TEXT("Per-tile estimate exceeds budget. Tune DensityFilter.LowerBound +0.1 or SurfaceSampler.PointsPerSquaredMeter /2, then pcg_partition_preview again.")
			: TEXT("Budget OK. If tiles_total > 4, use the pcg_incremental_fill DAG template; otherwise pcg_safe_generate."));

	OutSummary = FString::Printf(
		TEXT("Preview '%s': %d tiles × %.0f pts (~%.0f total), %d batches of %d, over_budget=%s"),
		*AssetPath, TilesTotal, PerTileEstimated, EstimatedTotalPoints, BatchesTotal, BatchSize,
		bOverBudget ? TEXT("true") : TEXT("false"));
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// A6: pcg_biome_overlay_apply — V2 with real write path + V1 probe fallback.
//
// V2 strategy (this ship):
//   1. Enumerate UPCGComponent(s) on the target actor (unchanged from V1).
//   2. For each component's UPCGGraphInterface, locate its user parameter bag:
//      the graph instance owns a FInstancedPropertyBag (available since UE 5.3)
//      via a UPROPERTY typically named 'ParametersOverrides' or similar. We find
//      it by reflection — walking FStructProperty fields and matching the struct
//      type to FInstancedPropertyBag::StaticStruct() — so we don't depend on an
//      exact property name that could change between PCG plugin versions.
//   3. For each key/value in `overrides`, call FInstancedPropertyBag::
//      SetValueSerializedString(). That API parses the string in the same way
//      ImportText does and routes it to the correctly-typed underlying property,
//      so we can accept numbers / booleans / strings uniformly.
//   4. If a graph instance has no property bag (blueprint-only graph, older
//      version, etc) we fall through to probe mode for that component — report
//      discovery + echo requested overrides with `applied: false` and reason.
//   5. Response.mode is 'write_v2' if every override was written on at least one
//      component, 'write_v2_partial' if some failed, or 'probe_v1' if no bag
//      was found anywhere. Per-override outcomes live in pcg_components[].
//
// The mode string contract is the same as before: callers (B3 composer, C2 DAG)
// switch on it to decide between overlay path and sandwich fallback.
// ═══════════════════════════════════════════════════════════════════════════════
namespace
{
	// Serialize a JSON value into the string form FInstancedPropertyBag's
	// SetValueSerializedString expects. Numbers and bools go as literals;
	// strings pass through verbatim; objects/arrays fall back to compact JSON
	// (callers shouldn't pass those for scalar properties, but don't crash).
	static FString JsonValueToWritableString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->Type == EJson::Null) return FString();
		if (Value->Type == EJson::Boolean) return Value->AsBool() ? TEXT("true") : TEXT("false");
		if (Value->Type == EJson::Number)
		{
			const double D = Value->AsNumber();
			// Preserve integer-ness when possible — many PCG parameters are int32.
			if (FMath::IsFinite(D) && FMath::Floor(D) == D && FMath::Abs(D) < 1.0e15)
			{
				return FString::Printf(TEXT("%lld"), static_cast<int64>(D));
			}
			return FString::SanitizeFloat(D);
		}
		if (Value->Type == EJson::String) return Value->AsString();
		// Object / Array: compact JSON stringification so the caller sees the
		// literal attempt; SetValueSerializedString will likely reject it and
		// our per-override result will show the reason.
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
		return Out;
	}

	// Find the graph instance's FInstancedPropertyBag.
	// Round 2 (Apr 2026): UE 5.7.4 wraps the bag inside FPCGOverrideInstancedPropertyBag,
	// so the old TFieldIterator<FStructProperty> search for FInstancedPropertyBag
	// never matched and A6 V2 biome overlay always degraded to sandwich fallback
	// (PCGGraph.h:127, 624, 752 confirms the right entry point is
	// UPCGGraphInterface::GetMutableUserParametersStruct_Unsafe()).
	// Strategy: try the proper PCG API first; keep the reflection probe as a
	// fallback for any future graph wrappers we don't recognize yet.
	static FInstancedPropertyBag* FindGraphParameterBag(UObject* GraphInstanceObj)
	{
		if (!GraphInstanceObj) return nullptr;

		// Primary path: UE 5.7+ PCG public API. Returns the live bag pointer
		// owned by the graph instance.
		if (UPCGGraphInterface* GraphInterface = Cast<UPCGGraphInterface>(GraphInstanceObj))
		{
			if (FInstancedPropertyBag* Bag = GraphInterface->GetMutableUserParametersStruct_Unsafe())
			{
				return Bag;
			}
		}

		// Fallback: reflection probe for unknown future wrappers. Keeps the
		// older behaviour for any object that isn't a UPCGGraphInterface.
		UClass* Cls = GraphInstanceObj->GetClass();
		if (!Cls) return nullptr;
		UScriptStruct* BagStruct = TBaseStructure<FInstancedPropertyBag>::Get();
		if (!BagStruct) return nullptr;

		for (TFieldIterator<FStructProperty> It(Cls); It; ++It)
		{
			FStructProperty* SP = *It;
			if (!SP || !SP->Struct) continue;
			if (SP->Struct == BagStruct || SP->Struct->IsChildOf(BagStruct))
			{
				return SP->ContainerPtrToValuePtr<FInstancedPropertyBag>(GraphInstanceObj);
			}
		}
		return nullptr;
	}

	// Describe the EPropertyBagResult enum numerically — we don't depend on
	// a specific 5.7 header export of the enum's string labels.
	static FString PropertyBagResultToString(EPropertyBagResult R)
	{
		switch (R)
		{
		case EPropertyBagResult::Success:            return TEXT("success");
		case EPropertyBagResult::PropertyNotFound:   return TEXT("property_not_found");
		case EPropertyBagResult::TypeMismatch:       return TEXT("type_mismatch");
		case EPropertyBagResult::OutOfBounds:        return TEXT("out_of_bounds");
		default: break;
		}
		return FString::Printf(TEXT("result_code_%d"), static_cast<int32>(R));
	}
} // namespace

static bool Tool_PcgBiomeOverlayApply(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString ActorId;
	if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
	{
		OutError = TEXT("Missing required argument: actor (target actor label or name carrying a UPCGComponent)");
		return false;
	}

	AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
	if (!Actor) return false;

	bool bDryRunOnly = false;
	Arguments->TryGetBoolField(TEXT("dry_run"), bDryRunOnly);

	const TSharedPtr<FJsonObject>* OverridesPtr = nullptr;
	Arguments->TryGetObjectField(TEXT("overrides"), OverridesPtr);
	TSharedRef<FJsonObject> EchoOverrides = MakeShared<FJsonObject>();
	int32 OverrideRequestCount = 0;
	if (OverridesPtr && OverridesPtr->IsValid())
	{
		for (const auto& P : (*OverridesPtr)->Values)
		{
			EchoOverrides->SetField(FString(*P.Key), P.Value);
			++OverrideRequestCount;
		}
	}

	FString BiomePreset;
	Arguments->TryGetStringField(TEXT("biome_preset"), BiomePreset);

	TArray<UPCGComponent*> PcgComps;
	Actor->GetComponents<UPCGComponent>(PcgComps);

	// Aggregate accounting across all components on the actor.
	int32 TotalWriteAttempts       = 0;
	int32 TotalWritesSucceeded     = 0;
	int32 TotalWritesFailed        = 0;
	int32 ComponentsWithBag        = 0;
	int32 ComponentsWithoutBag     = 0;

	TArray<TSharedPtr<FJsonValue>> CompReports;
	for (UPCGComponent* Comp : PcgComps)
	{
		if (!Comp) continue;
		TSharedRef<FJsonObject> CompJson = MakeShared<FJsonObject>();
		CompJson->SetStringField(TEXT("component_name"), Comp->GetName());
		CompJson->SetBoolField(TEXT("is_partitioned"), Comp->IsPartitioned());

		UPCGGraphInterface* GI = Comp->GetGraphInstance();
		UObject* GIObj = Cast<UObject>(GI);
		FString GraphPath;
		if (GI && GI->GetGraph()) GraphPath = GI->GetGraph()->GetPathName();
		CompJson->SetStringField(TEXT("graph_path"), GraphPath);

		if (!GI || !GIObj)
		{
			CompJson->SetStringField(TEXT("graph_note"), TEXT("no graph instance attached"));
			CompJson->SetBoolField(TEXT("has_parameter_bag"), false);
			CompJson->SetStringField(TEXT("component_mode"), TEXT("probe_v1"));
			++ComponentsWithoutBag;
			CompReports.Add(MakeShared<FJsonValueObject>(CompJson));
			continue;
		}

		FInstancedPropertyBag* Bag = FindGraphParameterBag(GIObj);
		if (!Bag)
		{
			CompJson->SetStringField(TEXT("graph_note"),
				TEXT("no FInstancedPropertyBag field found on graph instance — overlay falls back to probe"));
			CompJson->SetBoolField(TEXT("has_parameter_bag"), false);
			CompJson->SetStringField(TEXT("component_mode"), TEXT("probe_v1"));
			++ComponentsWithoutBag;
			CompReports.Add(MakeShared<FJsonValueObject>(CompJson));
			continue;
		}

		++ComponentsWithBag;
		CompJson->SetBoolField(TEXT("has_parameter_bag"), true);

		// We're about to mutate — make the undo system aware (no-op at runtime).
		if (!bDryRunOnly) GIObj->Modify();

		TArray<TSharedPtr<FJsonValue>> OverrideResults;
		int32 CompOk = 0, CompFail = 0;

		if (OverridesPtr && OverridesPtr->IsValid())
		{
			for (const auto& P : (*OverridesPtr)->Values)
			{
				++TotalWriteAttempts;
				const FString Key(*P.Key);
				const FName ParamName(*Key);
				const FString ValueStr = JsonValueToWritableString(P.Value);

				TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
				R->SetStringField(TEXT("property"), Key);
				R->SetStringField(TEXT("attempted_value"), ValueStr);

				if (bDryRunOnly)
				{
					const FPropertyBagPropertyDesc* Desc = Bag->FindPropertyDescByName(ParamName);
					const bool bExists = (Desc != nullptr);
					R->SetBoolField(TEXT("applied"), false);
					R->SetStringField(TEXT("result"), bExists ? TEXT("dry_run_would_write") : TEXT("property_not_found"));
					if (bExists) ++CompOk; else ++CompFail;
				}
				else
				{
					const EPropertyBagResult Result = Bag->SetValueSerializedString(ParamName, ValueStr);
					const bool bOk = (Result == EPropertyBagResult::Success);
					R->SetBoolField(TEXT("applied"), bOk);
					R->SetStringField(TEXT("result"), PropertyBagResultToString(Result));
					if (bOk) { ++CompOk; ++TotalWritesSucceeded; }
					else     { ++CompFail; ++TotalWritesFailed; }
				}
				OverrideResults.Add(MakeShared<FJsonValueObject>(R));
			}
		}

		CompJson->SetArrayField(TEXT("override_results"), OverrideResults);
		CompJson->SetNumberField(TEXT("override_applied_count"), CompOk);
		CompJson->SetNumberField(TEXT("override_failed_count"),  CompFail);

		if (!bDryRunOnly && CompOk > 0)
		{
			// Kick the usual edit-notification path so dependent systems see the
			// new values. These are safe no-ops if the instance isn't in an
			// editor world.
			GIObj->MarkPackageDirty();
			GIObj->PostEditChange();
			Comp->MarkPackageDirty();
		}

		CompJson->SetStringField(TEXT("component_mode"),
			bDryRunOnly ? TEXT("write_v2_dry_run")
			            : (CompFail == 0 ? TEXT("write_v2") :
			               (CompOk == 0 ? TEXT("write_v2_all_failed") : TEXT("write_v2_partial"))));
		CompReports.Add(MakeShared<FJsonValueObject>(CompJson));
	}

	// Top-level mode: aggregate contract the DAG fallbacks / B3 composer read.
	FString Mode;
	if (OverrideRequestCount == 0)
	{
		Mode = ComponentsWithBag > 0 ? TEXT("write_v2_noop") : TEXT("probe_v1");
	}
	else if (bDryRunOnly)
	{
		Mode = TEXT("write_v2_dry_run");
	}
	else if (TotalWriteAttempts == 0 || ComponentsWithBag == 0)
	{
		// No component had a bag — classic probe_v1 fallback (B3 will sandwich).
		Mode = TEXT("probe_v1");
	}
	else if (TotalWritesFailed == 0)
	{
		Mode = TEXT("write_v2");
	}
	else if (TotalWritesSucceeded > 0)
	{
		Mode = TEXT("write_v2_partial");
	}
	else
	{
		Mode = TEXT("write_v2_all_failed");
	}

	const bool bAnythingApplied = !bDryRunOnly && TotalWritesSucceeded > 0;

	OutStructured->SetStringField(TEXT("actor"), ActorId);
	OutStructured->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	OutStructured->SetNumberField(TEXT("pcg_component_count"), PcgComps.Num());
	OutStructured->SetNumberField(TEXT("components_with_parameter_bag"), ComponentsWithBag);
	OutStructured->SetNumberField(TEXT("components_without_parameter_bag"), ComponentsWithoutBag);
	OutStructured->SetArrayField(TEXT("pcg_components"), CompReports);
	OutStructured->SetStringField(TEXT("biome_preset"), BiomePreset);
	OutStructured->SetObjectField(TEXT("requested_overrides"), EchoOverrides);
	OutStructured->SetNumberField(TEXT("requested_override_count"), OverrideRequestCount);
	OutStructured->SetNumberField(TEXT("write_attempt_count"), TotalWriteAttempts);
	OutStructured->SetNumberField(TEXT("write_success_count"), TotalWritesSucceeded);
	OutStructured->SetNumberField(TEXT("write_failure_count"), TotalWritesFailed);
	OutStructured->SetBoolField(TEXT("applied"), bAnythingApplied);
	OutStructured->SetBoolField(TEXT("dry_run"), bDryRunOnly);
	OutStructured->SetStringField(TEXT("mode"), Mode);
	OutStructured->SetStringField(
		TEXT("note"),
		TEXT("V2 graph-instance overlay: writes overrides into the FInstancedPropertyBag carried by the "
		     "graph instance via SetValueSerializedString. On components without a property bag (old graph "
		     "or blueprint-authored), this call degrades to probe mode — B3 composer's sandwich fallback "
		     "(snapshot + pcg_graph_set_node_property + restore) is still the right path for those."));

	OutSummary = FString::Printf(
		TEXT("BiomeOverlay(%s): actor='%s' comps=%d(with_bag=%d) overrides=%d applied=%d failed=%d mode=%s"),
		bDryRunOnly ? TEXT("dry_run") : TEXT("write"),
		*ActorId, PcgComps.Num(), ComponentsWithBag, OverrideRequestCount,
		TotalWritesSucceeded, TotalWritesFailed, *Mode);
	if (!bDryRunOnly && OverrideRequestCount > 0 && TotalWritesFailed > 0)
	{
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("overrides"),
			TEXT("One or more biome override writes failed; caller should use graph snapshot/set/restore fallback."));
		OutError = FString::Printf(TEXT("Biome overlay wrote %d/%d attempted overrides; %d failed (mode=%s)."),
			TotalWritesSucceeded, TotalWriteAttempts, TotalWritesFailed, *Mode);
		return false;
	}
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// A7: pcg_graph_diff — structural + property delta between two PCG Graph assets.
//
// Intended users:
//   • pcg_graph_authoring_loop (C3) — decide whether an iteration introduced
//     meaningful change before another validate round-trip.
//   • pcg-biome-composer (B3) — compare a base graph vs a biome-composed
//     variant and report which nodes/parameters the composer actually changed.
//   • qa_inspector — drive a human-readable change summary in receipts.
//
// Node identity  = UPCGNode->GetName() (stable across pcg_graph_snapshot).
// Edge identity  = (from_node, from_pin, to_node, to_pin) tuple.
// Property diff  = iterate FProperties on each Settings UObject in common
//                  nodes, compare via ExportText_Direct, cap per node.
// ═══════════════════════════════════════════════════════════════════════════════
namespace
{
	// Canonical edge key: "fromNode:fromPin -> toNode:toPin"
	static FString MakeEdgeKey(const UPCGEdge* Edge)
	{
		if (!Edge) return FString();
		const UPCGPin* Src = Edge->InputPin.Get();   // upstream (data enters edge here)
		const UPCGPin* Dst = Edge->OutputPin.Get();  // downstream (data exits edge here)
		const FString SrcNode = (Src && Src->Node) ? Src->Node->GetName() : TEXT("x");
		const FString SrcPin  = Src ? Src->Properties.Label.ToString() : TEXT("x");
		const FString DstNode = (Dst && Dst->Node) ? Dst->Node->GetName() : TEXT("x");
		const FString DstPin  = Dst ? Dst->Properties.Label.ToString() : TEXT("x");
		return FString::Printf(TEXT("%s:%s -> %s:%s"), *SrcNode, *SrcPin, *DstNode, *DstPin);
	}

	static TSharedRef<FJsonObject> MakeEdgeJson(const UPCGEdge* Edge)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		if (!Edge) { O->SetStringField(TEXT("edge"), TEXT("null")); return O; }
		const UPCGPin* Src = Edge->InputPin.Get();
		const UPCGPin* Dst = Edge->OutputPin.Get();
		O->SetStringField(TEXT("from_node"), (Src && Src->Node) ? Src->Node->GetName() : TEXT(""));
		O->SetStringField(TEXT("from_pin"),  Src ? Src->Properties.Label.ToString() : TEXT(""));
		O->SetStringField(TEXT("to_node"),   (Dst && Dst->Node) ? Dst->Node->GetName() : TEXT(""));
		O->SetStringField(TEXT("to_pin"),    Dst ? Dst->Properties.Label.ToString() : TEXT(""));
		return O;
	}

	// Collect all outgoing edges of a graph into a name-keyed map.
	static void CollectEdges(const UPCGGraph* Graph, TMap<FString, const UPCGEdge*>& OutMap)
	{
		if (!Graph) return;
		for (const UPCGNode* Node : Graph->GetNodes())
		{
			if (!Node) continue;
			for (const UPCGPin* OutPin : Node->GetOutputPins())
			{
				if (!OutPin) continue;
				for (const TObjectPtr<UPCGEdge>& Edge : OutPin->Edges)
				{
					if (!Edge) continue;
					OutMap.Add(MakeEdgeKey(Edge.Get()), Edge.Get());
				}
			}
		}
	}

	// Export a single property's value as a string. Returns empty on failure.
	static FString ExportPropertyValueAsString(FProperty* Prop, const void* ContainerPtr)
	{
		if (!Prop || !ContainerPtr) return FString();
		FString Out;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(ContainerPtr);
		Prop->ExportText_Direct(Out, ValuePtr, ValuePtr, nullptr, PPF_None);
		return Out;
	}

	// Diff properties of two settings UObjects of the same class. Caps diffs.
	// Returns number of diffs appended to OutDiffs (which is an array of
	// {node, property, base_value, head_value} JSON objects).
	static int32 DiffSettingsProperties(
		const FString& NodeName,
		const UPCGSettings* BaseSettings,
		const UPCGSettings* HeadSettings,
		TArray<TSharedPtr<FJsonValue>>& OutDiffs,
		int32 MaxPerNode)
	{
		if (!BaseSettings || !HeadSettings) return 0;
		UClass* BaseCls = BaseSettings->GetClass();
		UClass* HeadCls = HeadSettings->GetClass();
		if (BaseCls != HeadCls)
		{
			// Class mismatch itself is one diff.
			TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetStringField(TEXT("node"), NodeName);
			D->SetStringField(TEXT("property"), TEXT("__class__"));
			D->SetStringField(TEXT("base_value"), BaseCls ? BaseCls->GetName() : TEXT(""));
			D->SetStringField(TEXT("head_value"), HeadCls ? HeadCls->GetName() : TEXT(""));
			OutDiffs.Add(MakeShared<FJsonValueObject>(D));
			return 1;
		}

		int32 DiffsAdded = 0;
		for (TFieldIterator<FProperty> It(BaseCls); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop) continue;
			// Skip transient / editor-only / delegate properties — they're not meaningful for authoring delta.
			if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly)) continue;
			if (Prop->IsA(FDelegateProperty::StaticClass()) ||
			    Prop->IsA(FMulticastDelegateProperty::StaticClass())) continue;

			const FString BaseVal = ExportPropertyValueAsString(Prop, BaseSettings);
			const FString HeadVal = ExportPropertyValueAsString(Prop, HeadSettings);
			if (BaseVal.Equals(HeadVal)) continue;

			TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetStringField(TEXT("node"), NodeName);
			D->SetStringField(TEXT("property"), Prop->GetName());
			D->SetStringField(TEXT("base_value"), BaseVal);
			D->SetStringField(TEXT("head_value"), HeadVal);
			OutDiffs.Add(MakeShared<FJsonValueObject>(D));
			++DiffsAdded;

			if (MaxPerNode > 0 && DiffsAdded >= MaxPerNode)
			{
				TSharedRef<FJsonObject> Note = MakeShared<FJsonObject>();
				Note->SetStringField(TEXT("node"), NodeName);
				Note->SetStringField(TEXT("property"), TEXT("__truncated__"));
				Note->SetStringField(TEXT("base_value"), TEXT(""));
				Note->SetStringField(TEXT("head_value"),
					FString::Printf(TEXT("additional diffs omitted (cap=%d)"), MaxPerNode));
				OutDiffs.Add(MakeShared<FJsonValueObject>(Note));
				break;
			}
		}
		return DiffsAdded;
	}
} // namespace

static bool Tool_PcgGraphDiff(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString BasePath, HeadPath;
	if (!Arguments->TryGetStringField(TEXT("base_asset_path"), BasePath) || BasePath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: base_asset_path");
		return false;
	}
	if (!Arguments->TryGetStringField(TEXT("head_asset_path"), HeadPath) || HeadPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: head_asset_path");
		return false;
	}

	bool bIncludeProps = true;
	Arguments->TryGetBoolField(TEXT("include_property_diff"), bIncludeProps);

	double MaxPropDiffsD = 10.0;
	Arguments->TryGetNumberField(TEXT("max_property_diffs_per_node"), MaxPropDiffsD);
	const int32 MaxPropDiffsPerNode = FMath::Clamp(static_cast<int32>(MaxPropDiffsD), 0, 500);

	UPCGGraph* Base = LoadPCGGraph(Context.Services, BasePath, OutError);
	if (!Base) return false;
	UPCGGraph* Head = LoadPCGGraph(Context.Services, HeadPath, OutError);
	if (!Head) return false;

	// ── Node sets ──────────────────────────────────────────────────────────
	TMap<FString, const UPCGNode*> BaseNodes, HeadNodes;
	for (const UPCGNode* N : Base->GetNodes()) { if (N) BaseNodes.Add(N->GetName(), N); }
	for (const UPCGNode* N : Head->GetNodes()) { if (N) HeadNodes.Add(N->GetName(), N); }

	TArray<TSharedPtr<FJsonValue>> NodesAdded, NodesRemoved;
	TArray<FString> CommonNodeNames;

	for (const auto& Pair : HeadNodes)
	{
		if (!BaseNodes.Contains(Pair.Key))
		{
			TSharedRef<FJsonObject> N = MakeShared<FJsonObject>();
			N->SetStringField(TEXT("node"), Pair.Key);
			if (const UPCGSettings* S = Pair.Value->GetSettings())
			{
				N->SetStringField(TEXT("settings_class"), S->GetClass()->GetName());
				N->SetStringField(TEXT("category"), CategorizeSettings(S->GetClass()));
			}
			NodesAdded.Add(MakeShared<FJsonValueObject>(N));
		}
		else
		{
			CommonNodeNames.Add(Pair.Key);
		}
	}
	for (const auto& Pair : BaseNodes)
	{
		if (!HeadNodes.Contains(Pair.Key))
		{
			TSharedRef<FJsonObject> N = MakeShared<FJsonObject>();
			N->SetStringField(TEXT("node"), Pair.Key);
			if (const UPCGSettings* S = Pair.Value->GetSettings())
			{
				N->SetStringField(TEXT("settings_class"), S->GetClass()->GetName());
				N->SetStringField(TEXT("category"), CategorizeSettings(S->GetClass()));
			}
			NodesRemoved.Add(MakeShared<FJsonValueObject>(N));
		}
	}

	// ── Edge sets ──────────────────────────────────────────────────────────
	TMap<FString, const UPCGEdge*> BaseEdges, HeadEdges;
	CollectEdges(Base, BaseEdges);
	CollectEdges(Head, HeadEdges);

	TArray<TSharedPtr<FJsonValue>> EdgesAdded, EdgesRemoved;
	for (const auto& Pair : HeadEdges)
	{
		if (!BaseEdges.Contains(Pair.Key))
		{
			EdgesAdded.Add(MakeShared<FJsonValueObject>(MakeEdgeJson(Pair.Value)));
		}
	}
	for (const auto& Pair : BaseEdges)
	{
		if (!HeadEdges.Contains(Pair.Key))
		{
			EdgesRemoved.Add(MakeShared<FJsonValueObject>(MakeEdgeJson(Pair.Value)));
		}
	}

	// ── Property diffs on common nodes ─────────────────────────────────────
	TArray<TSharedPtr<FJsonValue>> PropDiffs;
	int32 NodesWithPropChanges = 0;
	if (bIncludeProps)
	{
		for (const FString& Name : CommonNodeNames)
		{
			const UPCGNode* BaseN = BaseNodes[Name];
			const UPCGNode* HeadN = HeadNodes[Name];
			const UPCGSettings* BaseS = BaseN ? BaseN->GetSettings() : nullptr;
			const UPCGSettings* HeadS = HeadN ? HeadN->GetSettings() : nullptr;
			const int32 Added = DiffSettingsProperties(Name, BaseS, HeadS, PropDiffs, MaxPropDiffsPerNode);
			if (Added > 0) ++NodesWithPropChanges;
		}
	}

	// ── Assemble response ──────────────────────────────────────────────────
	OutStructured->SetStringField(TEXT("base_asset_path"), BasePath);
	OutStructured->SetStringField(TEXT("head_asset_path"), HeadPath);

	OutStructured->SetNumberField(TEXT("base_node_count"), BaseNodes.Num());
	OutStructured->SetNumberField(TEXT("head_node_count"), HeadNodes.Num());
	OutStructured->SetNumberField(TEXT("common_node_count"), CommonNodeNames.Num());
	OutStructured->SetNumberField(TEXT("base_edge_count"), BaseEdges.Num());
	OutStructured->SetNumberField(TEXT("head_edge_count"), HeadEdges.Num());

	OutStructured->SetArrayField(TEXT("nodes_added"),   NodesAdded);
	OutStructured->SetArrayField(TEXT("nodes_removed"), NodesRemoved);
	OutStructured->SetArrayField(TEXT("edges_added"),   EdgesAdded);
	OutStructured->SetArrayField(TEXT("edges_removed"), EdgesRemoved);
	OutStructured->SetArrayField(TEXT("property_diffs"), PropDiffs);

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("added_nodes"),   NodesAdded.Num());
	Summary->SetNumberField(TEXT("removed_nodes"), NodesRemoved.Num());
	Summary->SetNumberField(TEXT("added_edges"),   EdgesAdded.Num());
	Summary->SetNumberField(TEXT("removed_edges"), EdgesRemoved.Num());
	Summary->SetNumberField(TEXT("property_diff_count"), PropDiffs.Num());
	Summary->SetNumberField(TEXT("nodes_with_property_changes"), NodesWithPropChanges);
	Summary->SetBoolField(TEXT("include_property_diff"), bIncludeProps);
	OutStructured->SetObjectField(TEXT("summary"), Summary);

	const bool bChanged =
		NodesAdded.Num()   > 0 ||
		NodesRemoved.Num() > 0 ||
		EdgesAdded.Num()   > 0 ||
		EdgesRemoved.Num() > 0 ||
		PropDiffs.Num()    > 0;
	OutStructured->SetBoolField(TEXT("changed"), bChanged);

	OutSummary = FString::Printf(
		TEXT("Diff '%s' vs '%s': +%d/-%d nodes, +%d/-%d edges, %d property deltas on %d node(s) — %s"),
		*BasePath, *HeadPath,
		NodesAdded.Num(), NodesRemoved.Num(),
		EdgesAdded.Num(), EdgesRemoved.Num(),
		PropDiffs.Num(), NodesWithPropChanges,
		bChanged ? TEXT("CHANGED") : TEXT("IDENTICAL"));
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// pcg_resolve_graph_parameters — read-side companion to A6 V2 write.
//
// Given an actor, enumerate its UPCGComponent(s) and, for each graph instance
// that carries a FInstancedPropertyBag, dump every parameter's current value
// in serialized-string form. Pair with pcg_biome_overlay_apply(dry_run=true)
// to answer "what's actually overridable here, and what's it set to?" before
// touching anything.
//
// Response shape:
//   {
//     actor, actor_path, pcg_component_count,
//     pcg_components: [
//       { component_name, graph_path, is_partitioned,
//         has_parameter_bag: bool,
//         parameters: [ { name, type_name, value_string } ],
//         parameter_count }
//     ],
//     total_parameter_count
//   }
// ═══════════════════════════════════════════════════════════════════════════════
namespace
{
	// Mirror of PropertyBagResult → string from A6 V2.
	static const FPropertyBagPropertyDesc* FindPropertyDescSafe(const FInstancedPropertyBag& Bag, FName Name)
	{
		return Bag.FindPropertyDescByName(Name);
	}

	// Dump every property in a bag as (name, type_name, value_string).
	static int32 DumpBagParameters(const FInstancedPropertyBag* Bag, TArray<TSharedPtr<FJsonValue>>& Out)
	{
		if (!Bag) return 0;
		int32 Count = 0;
		const UPropertyBag* BagStruct = Bag->GetPropertyBagStruct();
		if (!BagStruct) return 0;

		for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
		{
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("name"), Desc.Name.ToString());
			if (Desc.CachedProperty)
			{
				J->SetStringField(TEXT("type_name"), Desc.CachedProperty->GetClass()->GetName());
			}
			// Attempt to read the value as a string. SetValueSerializedString has a
			// read cousin `GetValueSerializedString`; when not exposed, fall back
			// to FProperty::ExportText_Direct on the backing property.
			FString ValueStr;
			if (Desc.CachedProperty)
			{
				const void* ValuePtr = Bag->GetValue().GetMemory();
				if (ValuePtr)
				{
					const void* FieldPtr = Desc.CachedProperty->ContainerPtrToValuePtr<void>(ValuePtr);
					if (FieldPtr)
					{
						Desc.CachedProperty->ExportText_Direct(ValueStr, FieldPtr, FieldPtr, nullptr, PPF_None);
					}
				}
			}
			J->SetStringField(TEXT("value_string"), ValueStr);
			Out.Add(MakeShared<FJsonValueObject>(J));
			++Count;
		}
		return Count;
	}
} // namespace

static bool Tool_PcgResolveGraphParameters(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString ActorId;
	if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
	{
		OutError = TEXT("Missing required argument: actor");
		return false;
	}
	AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
	if (!Actor) return false;

	TArray<UPCGComponent*> PcgComps;
	Actor->GetComponents<UPCGComponent>(PcgComps);

	int32 TotalParamCount = 0;
	TArray<TSharedPtr<FJsonValue>> CompReports;
	for (UPCGComponent* Comp : PcgComps)
	{
		if (!Comp) continue;
		TSharedRef<FJsonObject> CompJson = MakeShared<FJsonObject>();
		CompJson->SetStringField(TEXT("component_name"), Comp->GetName());
		CompJson->SetBoolField(TEXT("is_partitioned"), Comp->IsPartitioned());

		UPCGGraphInterface* GI = Comp->GetGraphInstance();
		UObject* GIObj = Cast<UObject>(GI);
		if (GI && GI->GetGraph())
		{
			CompJson->SetStringField(TEXT("graph_path"), GI->GetGraph()->GetPathName());
		}

		FInstancedPropertyBag* Bag = GIObj ? FindGraphParameterBag(GIObj) : nullptr;
		CompJson->SetBoolField(TEXT("has_parameter_bag"), Bag != nullptr);

		TArray<TSharedPtr<FJsonValue>> Params;
		int32 ParamCount = DumpBagParameters(Bag, Params);
		CompJson->SetArrayField(TEXT("parameters"), Params);
		CompJson->SetNumberField(TEXT("parameter_count"), ParamCount);
		TotalParamCount += ParamCount;

		CompReports.Add(MakeShared<FJsonValueObject>(CompJson));
	}

	OutStructured->SetStringField(TEXT("actor"), ActorId);
	OutStructured->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	OutStructured->SetNumberField(TEXT("pcg_component_count"), PcgComps.Num());
	OutStructured->SetArrayField(TEXT("pcg_components"), CompReports);
	OutStructured->SetNumberField(TEXT("total_parameter_count"), TotalParamCount);

	OutSummary = FString::Printf(
		TEXT("ResolveParams: actor='%s' comps=%d total_params=%d"),
		*ActorId, PcgComps.Num(), TotalParamCount);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// pcg_component_info — one-shot state dump of a UPCGComponent on an actor.
//
// Combines graph path / is_generated / is_partitioned / last generate result
// into a single response so the AI doesn't need to stitch multiple tool calls.
// Use as a lightweight "what's the current state of this Volume?" query.
// ═══════════════════════════════════════════════════════════════════════════════
static bool Tool_PcgComponentInfo(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString ActorId;
	if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
	{
		OutError = TEXT("Missing required argument: actor");
		return false;
	}
	AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
	if (!Actor) return false;

	TArray<UPCGComponent*> PcgComps;
	Actor->GetComponents<UPCGComponent>(PcgComps);

	TArray<TSharedPtr<FJsonValue>> Reports;
	int32 GeneratedCount = 0, PartitionedCount = 0, WithBagCount = 0;
	for (UPCGComponent* Comp : PcgComps)
	{
		if (!Comp) continue;
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("component_name"), Comp->GetName());
		J->SetBoolField(TEXT("is_generated"), Comp->bGenerated);
		J->SetBoolField(TEXT("is_partitioned"), Comp->IsPartitioned());
		J->SetBoolField(TEXT("generate_on_load"), Comp->bActivated);

		UPCGGraphInterface* GI = Comp->GetGraphInstance();
		if (GI)
		{
			if (UPCGGraph* G = GI->GetGraph())
			{
				J->SetStringField(TEXT("graph_path"), G->GetPathName());
				J->SetNumberField(TEXT("graph_node_count"), G->GetNodes().Num());
			}
			UObject* GIObj = Cast<UObject>(GI);
			if (GIObj)
			{
				FInstancedPropertyBag* Bag = FindGraphParameterBag(GIObj);
				J->SetBoolField(TEXT("has_parameter_bag"), Bag != nullptr);
				if (Bag)
				{
					if (const UPropertyBag* BS = Bag->GetPropertyBagStruct())
					{
						J->SetNumberField(TEXT("parameter_count"), BS->GetPropertyDescs().Num());
					}
					++WithBagCount;
				}
			}
		}

		if (Comp->bGenerated)       ++GeneratedCount;
		if (Comp->IsPartitioned())  ++PartitionedCount;

		// Attached ISM/HISM count — a quick proxy for "how much was spawned".
		int32 IsmCount = 0, TotalInstances = 0;
		if (AActor* Owner = Comp->GetOwner())
		{
			TArray<UInstancedStaticMeshComponent*> Isms;
			Owner->GetComponents<UInstancedStaticMeshComponent>(Isms);
			IsmCount = Isms.Num();
			for (UInstancedStaticMeshComponent* I : Isms)
			{
				if (I) TotalInstances += I->GetInstanceCount();
			}
		}
		J->SetNumberField(TEXT("owner_ism_count"), IsmCount);
		J->SetNumberField(TEXT("owner_ism_total_instances"), TotalInstances);

		Reports.Add(MakeShared<FJsonValueObject>(J));
	}

	OutStructured->SetStringField(TEXT("actor"), ActorId);
	OutStructured->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	OutStructured->SetNumberField(TEXT("pcg_component_count"), PcgComps.Num());
	OutStructured->SetNumberField(TEXT("pcg_generated_count"), GeneratedCount);
	OutStructured->SetNumberField(TEXT("pcg_partitioned_count"), PartitionedCount);
	OutStructured->SetNumberField(TEXT("pcg_with_bag_count"), WithBagCount);
	OutStructured->SetArrayField(TEXT("pcg_components"), Reports);
	PcgExecutionSafety::AttachPcgEditorSubwindowCleanupEvidence(OutStructured, TEXT("pcg_component_info"));

	OutSummary = FString::Printf(
		TEXT("ComponentInfo: actor='%s' comps=%d generated=%d partitioned=%d with_bag=%d"),
		*ActorId, PcgComps.Num(), GeneratedCount, PartitionedCount, WithBagCount);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// A12: pcg_attribute_inspect — post-generate structural probe.
//
// PCG's authored-point attribute surface is only fully exposed while a graph is
// mid-execution. Once generation finishes, what you actually see in the level is
// the spawned Instanced/Hierarchical Static Mesh components (and their instance
// transforms). That is the surface this tool inspects:
//
//   • Per actor: find the UPCGComponent(s), report is_generated and partitioning.
//   • For each spawned ISM/HISM under the actor: mesh path + instance count +
//     a transform sample window (cap at sample_limit per component).
//   • Produce simple aggregate stats (total instances, per-mesh histogram).
//
// Callers: pcg_author / qa_inspector — "what did I actually spawn and where?"
// Pairs well with pcg_graph_diff (compare configured vs actual outcome).
// ═══════════════════════════════════════════════════════════════════════════════
namespace
{
	static TSharedRef<FJsonObject> TransformToJson(const FTransform& T)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		const FVector Loc = T.GetLocation();
		const FRotator Rot = T.Rotator();
		const FVector Scl = T.GetScale3D();
		TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
		L->SetNumberField(TEXT("x"), Loc.X); L->SetNumberField(TEXT("y"), Loc.Y); L->SetNumberField(TEXT("z"), Loc.Z);
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetNumberField(TEXT("pitch"), Rot.Pitch); R->SetNumberField(TEXT("yaw"), Rot.Yaw); R->SetNumberField(TEXT("roll"), Rot.Roll);
		TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetNumberField(TEXT("x"), Scl.X); S->SetNumberField(TEXT("y"), Scl.Y); S->SetNumberField(TEXT("z"), Scl.Z);
		J->SetObjectField(TEXT("location"), L);
		J->SetObjectField(TEXT("rotation"), R);
		J->SetObjectField(TEXT("scale"), S);
		return J;
	}
} // namespace

// UE 5.8 read-only runtime-generation scheduler probe. The 5.8 branch uses
// execution-source APIs added around the component-to-execution-source runtime
// gen migration; older engines intentionally return a guarded no-op receipt.
static bool Tool_PcgRuntimeGenSchedulerStatus(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& OutError)
{
#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 8)
	(void)Context;
	(void)Arguments;
	OutError.Reset();
	return ReturnPcg58Unavailable(TEXT("pcg_runtime_gen_scheduler_status"), OutStructured, OutSummary);
#else
	AttachPcg58ProbeBaseFields(OutStructured, TEXT("pcg_runtime_gen_scheduler_status"));
	AttachModuleAvailability(OutStructured);

	const bool bPcgModuleAvailable = IsModuleKnownOrLoaded(FName(TEXT("PCG")));
	if (!bPcgModuleAvailable)
	{
		OutStructured->SetBoolField(TEXT("available"), false);
		OutStructured->SetStringField(TEXT("status"), TEXT("unsupported"));
		OutStructured->SetStringField(TEXT("unsupported_reason"), TEXT("PCG module is not available in this editor session."));
		OutSummary = TEXT("pcg_runtime_gen_scheduler_status: unsupported (PCG module unavailable)");
		return true;
	}

	bool bIncludeComponents = false;
	Arguments->TryGetBoolField(TEXT("include_components"), bIncludeComponents);

	int32 MaxComponents = 64;
	Arguments->TryGetNumberField(TEXT("max_components"), MaxComponents);
	MaxComponents = FMath::Clamp(MaxComponents, 0, 512);

	FString ActorNamePattern;
	Arguments->TryGetStringField(TEXT("actor_name_pattern"), ActorNamePattern);

	FString WorldError;
	UWorld* World = Context.Services.GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError.IsEmpty() ? TEXT("No editor world is available.") : WorldError;
		return false;
	}

	UPCGSubsystem* PcgSubsystem = UPCGSubsystem::GetInstance(World);
	const bool bSubsystemAvailable = PcgSubsystem != nullptr;
	OutStructured->SetBoolField(TEXT("available"), bSubsystemAvailable);
	OutStructured->SetStringField(TEXT("world_name"), World->GetName());
	OutStructured->SetBoolField(TEXT("pcg_subsystem_available"), bSubsystemAvailable);

	int32 RegisteredExecutionSourceCount = 0;
	int32 RegisteredPartitionedExecutionSourceCount = 0;
	if (PcgSubsystem)
	{
		const TSet<IPCGGraphExecutionSource*> AllExecutionSources = PcgSubsystem->GetAllRegisteredExecutionSources();
		const TSet<IPCGGraphExecutionSource*> PartitionedExecutionSources = PcgSubsystem->GetAllRegisteredPartitionedExecutionSources();
		RegisteredExecutionSourceCount = AllExecutionSources.Num();
		RegisteredPartitionedExecutionSourceCount = PartitionedExecutionSources.Num();
		OutStructured->SetBoolField(TEXT("runtime_scheduler_present"), PcgSubsystem->GetRuntimeGenScheduler() != nullptr);
		OutStructured->SetBoolField(TEXT("gen_source_manager_present"), PcgSubsystem->GetGenSourceManager() != nullptr);
	}
	else
	{
		OutStructured->SetBoolField(TEXT("runtime_scheduler_present"), false);
		OutStructured->SetBoolField(TEXT("gen_source_manager_present"), false);
	}

	OutStructured->SetNumberField(TEXT("registered_execution_source_count"), RegisteredExecutionSourceCount);
	OutStructured->SetNumberField(TEXT("registered_partitioned_execution_source_count"), RegisteredPartitionedExecutionSourceCount);
	OutStructured->SetNumberField(TEXT("registered_nonpartitioned_execution_source_count"),
		FMath::Max(0, RegisteredExecutionSourceCount - RegisteredPartitionedExecutionSourceCount));

	int32 ActorCount = 0;
	int32 ComponentCount = 0;
	int32 RuntimeManagedComponentCount = 0;
	int32 ActorComponentlessRuntimeCount = 0;
	int32 GeneratingComponentCount = 0;
	int32 GeneratedComponentCount = 0;
	int32 PartitionedComponentCount = 0;
	int32 ActivatedComponentCount = 0;
	TArray<TSharedPtr<FJsonValue>> ComponentReports;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		const FString ActorName = Actor->GetName();
		const FString ActorPath = Actor->GetPathName();
#if WITH_EDITOR
		const FString ActorLabel = Actor->GetActorLabel();
#else
		const FString ActorLabel;
#endif
		if (!ActorNamePattern.IsEmpty()
			&& !ActorName.Contains(ActorNamePattern, ESearchCase::IgnoreCase)
			&& !ActorPath.Contains(ActorNamePattern, ESearchCase::IgnoreCase)
			&& !ActorLabel.Contains(ActorNamePattern, ESearchCase::IgnoreCase))
		{
			continue;
		}

		++ActorCount;
		TArray<UPCGComponent*> Components;
		Actor->GetComponents<UPCGComponent>(Components);
		for (UPCGComponent* Comp : Components)
		{
			if (!Comp)
			{
				continue;
			}

			++ComponentCount;
			const bool bRuntimeManaged = Comp->IsManagedByRuntimeGenSystem();
			const bool bGenerating = Comp->IsGenerating();
			const bool bGenerated = Comp->bGenerated;
			const bool bPartitioned = Comp->IsPartitioned();
			const bool bActorComponentless = Comp->UseActorComponentlessGeneration();

			if (bRuntimeManaged) { ++RuntimeManagedComponentCount; }
			if (bActorComponentless) { ++ActorComponentlessRuntimeCount; }
			if (bGenerating) { ++GeneratingComponentCount; }
			if (bGenerated) { ++GeneratedComponentCount; }
			if (bPartitioned) { ++PartitionedComponentCount; }
			if (Comp->bActivated) { ++ActivatedComponentCount; }

			if (bIncludeComponents && ComponentReports.Num() < MaxComponents)
			{
				TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
				Report->SetStringField(TEXT("actor_name"), ActorName);
				Report->SetStringField(TEXT("actor_label"), ActorLabel);
				Report->SetStringField(TEXT("actor_path"), ActorPath);
				Report->SetStringField(TEXT("component_name"), Comp->GetName());
				Report->SetStringField(TEXT("component_path"), Comp->GetPathName());
				Report->SetBoolField(TEXT("is_managed_by_runtime_gen_system"), bRuntimeManaged);
				Report->SetBoolField(TEXT("use_actor_componentless_generation"), bActorComponentless);
				Report->SetBoolField(TEXT("is_generating"), bGenerating);
				Report->SetBoolField(TEXT("is_generated"), bGenerated);
				Report->SetBoolField(TEXT("is_partitioned"), bPartitioned);
				Report->SetBoolField(TEXT("is_activated"), Comp->bActivated);
				Report->SetNumberField(TEXT("generation_grid_size"), static_cast<double>(Comp->GetGenerationGridSize()));
				if (UPCGGraphInterface* GraphInterface = Comp->GetGraphInstance())
				{
					if (UPCGGraph* Graph = GraphInterface->GetGraph())
					{
						Report->SetStringField(TEXT("graph_path"), Graph->GetPathName());
						Report->SetNumberField(TEXT("graph_node_count"), Graph->GetNodes().Num());
					}
				}
				ComponentReports.Add(MakeShared<FJsonValueObject>(Report));
			}
		}
	}

	OutStructured->SetNumberField(TEXT("scanned_actor_count"), ActorCount);
	OutStructured->SetNumberField(TEXT("pcg_component_count"), ComponentCount);
	OutStructured->SetNumberField(TEXT("runtime_managed_component_count"), RuntimeManagedComponentCount);
	OutStructured->SetNumberField(TEXT("actor_componentless_runtime_component_count"), ActorComponentlessRuntimeCount);
	OutStructured->SetNumberField(TEXT("generating_component_count"), GeneratingComponentCount);
	OutStructured->SetNumberField(TEXT("generated_component_count"), GeneratedComponentCount);
	OutStructured->SetNumberField(TEXT("partitioned_component_count"), PartitionedComponentCount);
	OutStructured->SetNumberField(TEXT("activated_component_count"), ActivatedComponentCount);
	OutStructured->SetBoolField(TEXT("component_rows_truncated"), bIncludeComponents && ComponentReports.Num() < ComponentCount);
	OutStructured->SetArrayField(TEXT("components"), ComponentReports);

	AttachConsoleVariableSnapshot(OutStructured, TEXT("runtime_generation_enable"), TEXT("pcg.RuntimeGeneration.Enable"));
	AttachConsoleVariableSnapshot(OutStructured, TEXT("runtime_generation_num_generating_components"), TEXT("pcg.RuntimeGeneration.NumGeneratingComponents"));
	AttachConsoleVariableSnapshot(OutStructured, TEXT("runtime_generation_global_radius_multiplier"), TEXT("pcg.RuntimeGeneration.GlobalRadiusMultiplier"));
	AttachConsoleVariableSnapshot(OutStructured, TEXT("runtime_generation_enable_pooling"), TEXT("pcg.RuntimeGeneration.EnablePooling"));
	AttachConsoleVariableSnapshot(OutStructured, TEXT("runtime_generation_base_pool_size"), TEXT("pcg.RuntimeGeneration.BasePoolSize"));
	AttachConsoleVariableSnapshot(OutStructured, TEXT("runtime_generation_scheduler_tick_interval"), TEXT("pcg.RuntimeGeneration.TimeBetweenRuntimeGenSchedulerTicks"));

	OutStructured->SetStringField(TEXT("status"), bSubsystemAvailable ? TEXT("available") : TEXT("unsupported"));
	if (!bSubsystemAvailable)
	{
		OutStructured->SetStringField(TEXT("unsupported_reason"), TEXT("UPCGSubsystem is not initialized for the current editor world."));
	}

	OutSummary = FString::Printf(
		TEXT("RuntimeGenSchedulerStatus: subsystem=%s sources=%d partitioned_sources=%d pcg_components=%d runtime_managed=%d generating=%d"),
		bSubsystemAvailable ? TEXT("available") : TEXT("missing"),
		RegisteredExecutionSourceCount,
		RegisteredPartitionedExecutionSourceCount,
		ComponentCount,
		RuntimeManagedComponentCount,
		GeneratingComponentCount);
	return true;
#endif
}

// UE 5.8 read-only compute graph compile/cache probe. It deliberately avoids
// invoking graph compilation or shader compilation; it only checks module
// availability, GPU node intent, and whether a cached compute graph is already
// present for the requested grid/index.
static bool Tool_PcgComputeGraphCompileProbe(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& OutError)
{
#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 8)
	(void)Context;
	(void)Arguments;
	OutError.Reset();
	return ReturnPcg58Unavailable(TEXT("pcg_compute_graph_compile_probe"), OutStructured, OutSummary);
#else
	AttachPcg58ProbeBaseFields(OutStructured, TEXT("pcg_compute_graph_compile_probe"));
	AttachModuleAvailability(OutStructured);
	OutStructured->SetBoolField(TEXT("compile_requested"), false);
	OutStructured->SetBoolField(TEXT("shader_compile_requested"), false);
	OutStructured->SetBoolField(TEXT("compile_cache_write_attempted"), false);

	const bool bPcgModuleAvailable = IsModuleKnownOrLoaded(FName(TEXT("PCG")));
	const bool bPcgComputeModuleAvailable = IsModuleKnownOrLoaded(FName(TEXT("PCGCompute")));
	if (!bPcgModuleAvailable || !bPcgComputeModuleAvailable)
	{
		OutStructured->SetBoolField(TEXT("available"), false);
		OutStructured->SetStringField(TEXT("status"), TEXT("unsupported"));
		OutStructured->SetStringField(TEXT("unsupported_reason"), TEXT("PCG or PCGCompute module is not available in this editor session."));
		OutSummary = TEXT("pcg_compute_graph_compile_probe: unsupported (PCG/PCGCompute module unavailable)");
		return true;
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	int32 GridSizeArg = 0;
	Arguments->TryGetNumberField(TEXT("grid_size"), GridSizeArg);
	const uint32 GridSize = GridSizeArg > 0 ? static_cast<uint32>(GridSizeArg) : 0u;

	int32 ComputeGraphIndex = 0;
	Arguments->TryGetNumberField(TEXT("compute_graph_index"), ComputeGraphIndex);
	ComputeGraphIndex = FMath::Max(0, ComputeGraphIndex);

	bool bIncludeNodes = true;
	Arguments->TryGetBoolField(TEXT("include_nodes"), bIncludeNodes);

	UPCGGraph* Graph = LoadPCGGraph(Context.Services, AssetPath, OutError);
	if (!Graph)
	{
		return false;
	}

	int32 NodeCount = 0;
	int32 SettingsMissingCount = 0;
	int32 GpuSettingVisibleCount = 0;
	int32 GpuExecutionRequestedCount = 0;
	int32 GpuTypeNodeCount = 0;
	TArray<TSharedPtr<FJsonValue>> NodeReports;

	for (UPCGNode* Node : Graph->GetNodes())
	{
		if (!Node)
		{
			continue;
		}

		++NodeCount;
		const UPCGSettings* Settings = Node->GetSettings();
		if (!Settings)
		{
			++SettingsMissingCount;
			if (bIncludeNodes)
			{
				TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
				Report->SetStringField(TEXT("node"), NodeDisplayName(Node));
				Report->SetStringField(TEXT("status"), TEXT("settings_missing"));
				NodeReports.Add(MakeShared<FJsonValueObject>(Report));
			}
			continue;
		}

		const bool bDisplayGpu = Settings->DisplayExecuteOnGPUSetting();
		const bool bShouldGpu = Settings->ShouldExecuteOnGPU();
		const bool bGpuType = Settings->GetType() == EPCGSettingsType::GPU;
		if (bDisplayGpu) { ++GpuSettingVisibleCount; }
		if (bShouldGpu) { ++GpuExecutionRequestedCount; }
		if (bGpuType) { ++GpuTypeNodeCount; }

		if (bIncludeNodes)
		{
			TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
			Report->SetStringField(TEXT("node"), NodeDisplayName(Node));
			Report->SetStringField(TEXT("settings_class"), Settings->GetClass() ? Settings->GetClass()->GetName() : TEXT(""));
			Report->SetStringField(TEXT("settings_category"), Settings->GetClass() ? CategorizeSettings(Settings->GetClass()) : TEXT("unknown"));
			Report->SetStringField(TEXT("settings_type"), PcgSettingsTypeToString(Settings->GetType()));
			Report->SetBoolField(TEXT("display_execute_on_gpu_setting"), bDisplayGpu);
			Report->SetBoolField(TEXT("should_execute_on_gpu"), bShouldGpu);
			Report->SetBoolField(TEXT("is_gpu_settings_type"), bGpuType);
			NodeReports.Add(MakeShared<FJsonValueObject>(Report));
		}
	}

	bool bSubsystemAvailable = false;
	bool bCachedComputeGraphPresent = false;
	FString CacheProbeNote;
	FString WorldError;
	if (UWorld* World = Context.Services.GetEditorWorld(WorldError))
	{
		if (UPCGSubsystem* PcgSubsystem = UPCGSubsystem::GetInstance(World))
		{
			bSubsystemAvailable = true;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			UPCGComputeGraph* CachedComputeGraph = PcgSubsystem->GetComputeGraph(Graph, GridSize, static_cast<uint32>(ComputeGraphIndex));
PRAGMA_ENABLE_DEPRECATION_WARNINGS
			bCachedComputeGraphPresent = CachedComputeGraph != nullptr;
		}
		else
		{
			CacheProbeNote = TEXT("UPCGSubsystem is not initialized for the current editor world; cache lookup skipped.");
		}
	}
	else
	{
		CacheProbeNote = WorldError.IsEmpty() ? TEXT("No editor world is available; cache lookup skipped.") : WorldError;
	}

	FString Status;
	if (GpuExecutionRequestedCount == 0 && GpuTypeNodeCount == 0)
	{
		Status = TEXT("no_gpu_compute_nodes_detected");
	}
	else if (bCachedComputeGraphPresent)
	{
		Status = TEXT("cached_compute_graph_present");
	}
	else
	{
		Status = TEXT("not_compiled_or_not_cached");
	}

	OutStructured->SetBoolField(TEXT("available"), true);
	OutStructured->SetStringField(TEXT("status"), Status);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("graph_path"), Graph->GetPathName());
	OutStructured->SetNumberField(TEXT("grid_size"), static_cast<double>(GridSize));
	OutStructured->SetNumberField(TEXT("compute_graph_index"), ComputeGraphIndex);
	OutStructured->SetNumberField(TEXT("node_count"), NodeCount);
	OutStructured->SetNumberField(TEXT("settings_missing_count"), SettingsMissingCount);
	OutStructured->SetNumberField(TEXT("gpu_setting_visible_count"), GpuSettingVisibleCount);
	OutStructured->SetNumberField(TEXT("gpu_execution_requested_count"), GpuExecutionRequestedCount);
	OutStructured->SetNumberField(TEXT("gpu_settings_type_count"), GpuTypeNodeCount);
	OutStructured->SetBoolField(TEXT("pcg_subsystem_available"), bSubsystemAvailable);
	OutStructured->SetBoolField(TEXT("cached_compute_graph_present"), bCachedComputeGraphPresent);
	if (!CacheProbeNote.IsEmpty())
	{
		OutStructured->SetStringField(TEXT("cache_probe_note"), CacheProbeNote);
	}
	OutStructured->SetArrayField(TEXT("nodes"), NodeReports);
	OutStructured->SetStringField(TEXT("next_safe_action"), TEXT("If status is not_compiled_or_not_cached, run a normal PCG validation path first; this probe does not compile or generate."));

	OutSummary = FString::Printf(
		TEXT("ComputeGraphCompileProbe '%s': status=%s nodes=%d gpu_requested=%d cached=%s"),
		*AssetPath,
		*Status,
		NodeCount,
		GpuExecutionRequestedCount,
		bCachedComputeGraphPresent ? TEXT("true") : TEXT("false"));
	return true;
#endif
}

static bool Tool_PcgAttributeInspect(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString ActorId;
	if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
	{
		OutError = TEXT("Missing required argument: actor (target actor that owns the spawned ISM/HISM children)");
		return false;
	}
	AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
	if (!Actor) return false;

	double SampleLimitD = 8.0;
	Arguments->TryGetNumberField(TEXT("sample_limit"), SampleLimitD);
	const int32 SampleLimit = FMath::Clamp(static_cast<int32>(SampleLimitD), 0, 256);

	bool bIncludeTransforms = true;
	Arguments->TryGetBoolField(TEXT("include_transforms"), bIncludeTransforms);

	// Optional mesh path filter (substring). Case-insensitive contains match.
	FString MeshFilter;
	Arguments->TryGetStringField(TEXT("mesh_path_contains"), MeshFilter);
	const bool bHaveFilter = !MeshFilter.IsEmpty();

	// V2 extension: include PerInstanceSMCustomData sampling. PCG's ActorSpawner /
	// InstanceSpawner writes computed per-point attributes (density, scale_variant,
	// material_index, biome_weight, …) into the ISM's PerInstanceSMCustomData flat
	// float array of length NumCustomDataFloats × InstanceCount. Reading this post-
	// generation is the only reliable way to see real PCG point attributes without
	// replaying the graph — and it survives on disk as part of the actor state.
	bool bIncludeCustomData = false;
	Arguments->TryGetBoolField(TEXT("include_custom_data"), bIncludeCustomData);
	double CustomDataSampleLimitD = 8.0;
	Arguments->TryGetNumberField(TEXT("custom_data_sample_limit"), CustomDataSampleLimitD);
	const int32 CustomDataSampleLimit = FMath::Clamp(static_cast<int32>(CustomDataSampleLimitD), 0, 256);

	// PCG component(s) on the actor — report generation state.
	TArray<UPCGComponent*> PcgComps;
	Actor->GetComponents<UPCGComponent>(PcgComps);
	TArray<TSharedPtr<FJsonValue>> PcgReports;
	for (UPCGComponent* Comp : PcgComps)
	{
		if (!Comp) continue;
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("component_name"), Comp->GetName());
		J->SetBoolField(TEXT("is_generated"), Comp->bGenerated);
		J->SetBoolField(TEXT("is_partitioned"), Comp->IsPartitioned());
		if (UPCGGraphInterface* GI = Comp->GetGraphInstance())
		{
			if (UPCGGraph* G = GI->GetGraph())
			{
				J->SetStringField(TEXT("graph_path"), G->GetPathName());
			}
		}
		PcgReports.Add(MakeShared<FJsonValueObject>(J));
	}

	// Walk every ISM/HISM owned by or attached to this actor.
	TArray<UInstancedStaticMeshComponent*> IsmComps;
	Actor->GetComponents<UInstancedStaticMeshComponent>(IsmComps);

	TArray<TSharedPtr<FJsonValue>> MeshReports;
	TMap<FString, int32> MeshHistogram;
	int32 TotalInstances = 0;
	int32 EmittedComponents = 0, SkippedByFilter = 0;

	for (UInstancedStaticMeshComponent* Ism : IsmComps)
	{
		if (!Ism) continue;
		const int32 InstanceCount = Ism->GetInstanceCount();
		UStaticMesh* Mesh = Ism->GetStaticMesh();
		const FString MeshPath = Mesh ? Mesh->GetPathName() : FString();

		if (bHaveFilter && !MeshPath.Contains(MeshFilter, ESearchCase::IgnoreCase))
		{
			++SkippedByFilter;
			continue;
		}

		TotalInstances += InstanceCount;
		const int32 Count = MeshHistogram.FindRef(MeshPath);
		MeshHistogram.Add(MeshPath, Count + InstanceCount);

		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("component_name"), Ism->GetName());
		J->SetStringField(TEXT("component_class"), Ism->GetClass()->GetName());
		J->SetBoolField(TEXT("is_hism"), Ism->IsA(UHierarchicalInstancedStaticMeshComponent::StaticClass()));
		J->SetStringField(TEXT("mesh_path"), MeshPath);
		J->SetNumberField(TEXT("instance_count"), InstanceCount);

		if (bIncludeTransforms && SampleLimit > 0 && InstanceCount > 0)
		{
			const int32 Take = FMath::Min(InstanceCount, SampleLimit);
			TArray<TSharedPtr<FJsonValue>> Samples;
			Samples.Reserve(Take);
			for (int32 I = 0; I < Take; ++I)
			{
				FTransform T;
				if (Ism->GetInstanceTransform(I, T, /*bWorldSpace=*/true))
				{
					Samples.Add(MakeShared<FJsonValueObject>(TransformToJson(T)));
				}
			}
			J->SetArrayField(TEXT("transform_samples"), Samples);
			if (InstanceCount > Take)
			{
				J->SetStringField(TEXT("transform_samples_note"),
					FString::Printf(TEXT("Sampled %d of %d instances (sample_limit=%d)"), Take, InstanceCount, SampleLimit));
			}
		}

		// V2: PerInstanceSMCustomData probe — real post-generate PCG point attribute view.
		// Layout is a flat float array of length NumCustomDataFloats × InstanceCount;
		// instance i's per-float block lives at [i*N, (i+1)*N). If the ISM was authored
		// without custom data (common for plain foliage), num_custom_data_floats=0 and
		// custom_data_samples is an empty array — still cheap to emit, no special-case.
		if (bIncludeCustomData)
		{
			const int32 NumFloats     = Ism->NumCustomDataFloats;
			const int32 CustomLen     = Ism->PerInstanceSMCustomData.Num();
			const int32 EffInstances  = (NumFloats > 0) ? (CustomLen / NumFloats) : 0;
			J->SetNumberField(TEXT("num_custom_data_floats"), NumFloats);
			J->SetNumberField(TEXT("custom_data_total_floats"), CustomLen);
			J->SetNumberField(TEXT("custom_data_instance_count"), EffInstances);
			J->SetBoolField(TEXT("custom_data_consistent"),
				(NumFloats == 0 && CustomLen == 0) ||
				(NumFloats > 0 && (CustomLen % NumFloats) == 0 && EffInstances == InstanceCount));

			if (NumFloats > 0 && CustomDataSampleLimit > 0 && EffInstances > 0)
			{
				const int32 Take = FMath::Min(EffInstances, CustomDataSampleLimit);
				TArray<TSharedPtr<FJsonValue>> CDSamples;
				CDSamples.Reserve(Take);
				// Also compute min/max/sum per float channel for quick shape inspection.
				TArray<double> ChanMin, ChanMax, ChanSum;
				ChanMin.Init(+FLT_MAX, NumFloats);
				ChanMax.Init(-FLT_MAX, NumFloats);
				ChanSum.Init(0.0, NumFloats);
				int32 StatsN = 0;

				for (int32 I = 0; I < Take; ++I)
				{
					TArray<TSharedPtr<FJsonValue>> Block;
					Block.Reserve(NumFloats);
					const int32 Base = I * NumFloats;
					for (int32 F = 0; F < NumFloats; ++F)
					{
						const int32 Idx = Base + F;
						if (Idx >= CustomLen) break;
						const float V = Ism->PerInstanceSMCustomData[Idx];
						Block.Add(MakeShared<FJsonValueNumber>(V));
						// Accumulate stats across all sampled instances.
						if (V < ChanMin[F]) ChanMin[F] = V;
						if (V > ChanMax[F]) ChanMax[F] = V;
						ChanSum[F] += V;
					}
					CDSamples.Add(MakeShared<FJsonValueArray>(Block));
					++StatsN;
				}
				J->SetArrayField(TEXT("custom_data_samples"), CDSamples);
				if (EffInstances > Take)
				{
					J->SetStringField(TEXT("custom_data_samples_note"),
						FString::Printf(TEXT("Sampled %d of %d instances (custom_data_sample_limit=%d)"),
							Take, EffInstances, CustomDataSampleLimit));
				}

				// Per-channel stats over the sampled window (not the full population —
				// that would defeat the sample cap). Useful for quick distribution smell.
				TArray<TSharedPtr<FJsonValue>> ChanStats;
				ChanStats.Reserve(NumFloats);
				for (int32 F = 0; F < NumFloats; ++F)
				{
					TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
					C->SetNumberField(TEXT("channel"), F);
					const double MinV = (StatsN > 0) ? ChanMin[F] : 0.0;
					const double MaxV = (StatsN > 0) ? ChanMax[F] : 0.0;
					const double Mean = (StatsN > 0) ? (ChanSum[F] / static_cast<double>(StatsN)) : 0.0;
					C->SetNumberField(TEXT("min"),  MinV);
					C->SetNumberField(TEXT("max"),  MaxV);
					C->SetNumberField(TEXT("mean"), Mean);
					C->SetNumberField(TEXT("sampled_instances"), StatsN);
					ChanStats.Add(MakeShared<FJsonValueObject>(C));
				}
				J->SetArrayField(TEXT("custom_data_channel_stats"), ChanStats);
			}
			else
			{
				// Explicit empty arrays keep downstream schema stable across variants.
				J->SetArrayField(TEXT("custom_data_samples"), TArray<TSharedPtr<FJsonValue>>());
				J->SetArrayField(TEXT("custom_data_channel_stats"), TArray<TSharedPtr<FJsonValue>>());
			}
		}

		MeshReports.Add(MakeShared<FJsonValueObject>(J));
		++EmittedComponents;
	}

	// Build per-mesh histogram array (sorted descending by count).
	struct FEntry { FString Path; int32 Count; };
	TArray<FEntry> HistArr;
	HistArr.Reserve(MeshHistogram.Num());
	for (const auto& P : MeshHistogram) HistArr.Add({P.Key, P.Value});
	HistArr.Sort([](const FEntry& A, const FEntry& B){ return A.Count > B.Count; });
	TArray<TSharedPtr<FJsonValue>> HistJson;
	for (const FEntry& E : HistArr)
	{
		TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
		H->SetStringField(TEXT("mesh_path"), E.Path);
		H->SetNumberField(TEXT("instance_count"), E.Count);
		HistJson.Add(MakeShared<FJsonValueObject>(H));
	}

	OutStructured->SetStringField(TEXT("actor"), ActorId);
	OutStructured->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	OutStructured->SetArrayField(TEXT("pcg_components"), PcgReports);
	OutStructured->SetNumberField(TEXT("ism_component_count"), IsmComps.Num());
	OutStructured->SetNumberField(TEXT("ism_components_reported"), EmittedComponents);
	OutStructured->SetNumberField(TEXT("ism_components_skipped_by_filter"), SkippedByFilter);
	OutStructured->SetArrayField(TEXT("ism_components"), MeshReports);
	OutStructured->SetNumberField(TEXT("total_instance_count"), TotalInstances);
	OutStructured->SetArrayField(TEXT("per_mesh_histogram"), HistJson);
	OutStructured->SetNumberField(TEXT("distinct_mesh_count"), MeshHistogram.Num());
	if (bHaveFilter) OutStructured->SetStringField(TEXT("mesh_path_filter"), MeshFilter);
	PcgExecutionSafety::AttachPcgEditorSubwindowCleanupEvidence(OutStructured, TEXT("pcg_attribute_inspect"));

	// V2 summary augmentation: count ISMs that carry PCG point attributes (custom data).
	int32 IsmWithCustomData = 0;
	int32 MaxCustomChannels = 0;
	if (bIncludeCustomData)
	{
		for (UInstancedStaticMeshComponent* Ism : IsmComps)
		{
			if (!Ism) continue;
			if (Ism->NumCustomDataFloats > 0 && Ism->PerInstanceSMCustomData.Num() > 0)
			{
				++IsmWithCustomData;
				if (Ism->NumCustomDataFloats > MaxCustomChannels)
					MaxCustomChannels = Ism->NumCustomDataFloats;
			}
		}
		OutStructured->SetNumberField(TEXT("ism_with_custom_data"), IsmWithCustomData);
		OutStructured->SetNumberField(TEXT("max_custom_data_channels"), MaxCustomChannels);
		OutStructured->SetBoolField(TEXT("include_custom_data"), true);
	}

	OutSummary = FString::Printf(
		TEXT("AttributeInspect: actor='%s' pcg=%d ism=%d(emitted=%d) total_instances=%d distinct_meshes=%d cd_isms=%d cd_chans_max=%d"),
		*ActorId, PcgComps.Num(), IsmComps.Num(), EmittedComponents, TotalInstances, MeshHistogram.Num(),
		IsmWithCustomData, MaxCustomChannels);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// pcg_character_montage_decorate — post-PCG character decoration.
//
// PCG's ActorSpawner can scatter SkeletalMesh-bearing actors (NPCs, crowd fill,
// animals). What PCG itself *cannot* do is pick an AnimMontage and play it per
// spawned actor — that decision lives in the Animation system. This tool bridges
// the gap: given an actor (the PCG Volume itself, the world, or a parent actor),
// it walks candidate skeletal actors, picks a montage from a weighted pool using
// a deterministic seed, and either assigns-only or also plays the montage.
//
// Typical pipeline:
//   1. pcg_generate / pcg_generate_async on a Volume whose graph spawns NPCs.
//   2. pcg_character_montage_decorate(volume_actor=V, montage_pool=[...]).
//   3. Receipt records per-actor assignment; AI can diff across runs via seed.
//
// Inputs:
//   actor              required — PCG Volume or scope actor; we scan its
//                      attached + children actors for USkeletalMeshComponents.
//   actor_name_pattern optional substring filter on candidate actor names.
//   montage_pool       array of { path, weight (default 1.0),
//                                 play_rate_min (default 1.0),
//                                 play_rate_max (default 1.0) }.
//                      Weights are normalized. Zero or negative weight = skip.
//   seed               int, deterministic RNG seed (default 0).
//   assign_only        bool — if true, persist montage reference for later play but
//                      don't Montage_Play. Default false = play immediately.
//   max_actors         cap on actors to decorate in one call (default 256).
//   apply_to           "children" (default) | "attached" | "all" — how to find
//                      candidate actors starting from `actor`.
//
// Behavior:
//   • Pool with no valid weights → bail with clear error.
//   • Each actor picked gets one montage; Montage_Play uses the sampled play_rate.
//   • On actors without SkeletalMeshComponent or AnimInstance → skipped with
//     reason 'no_skeletal_mesh_component' / 'no_anim_instance'.
//   • Receipt per-actor: { actor, montage_path, play_rate, played: bool,
//     durable_binding, reason? }.
// ═══════════════════════════════════════════════════════════════════════════════
namespace
{
	struct FMontagePoolEntry
	{
		FString Path;
		float   Weight        = 1.0f;
		float   PlayRateMin   = 1.0f;
		float   PlayRateMax   = 1.0f;
		int32   RawIndex      = -1;
		int32   PoolIndex     = -1;
	};

	static constexpr TCHAR GMontageSamplerVersion[] = TEXT("splitmix64_v1");
	static constexpr uint64 GMontageSplitMixC1 = 0xbf58476d1ce4e5b9ULL;
	static constexpr uint64 GMontageSplitMixC2 = 0x94d049bb133111ebULL;
	static constexpr uint64 GMontageSplitMixGamma = 0x9e3779b97f4a7c15ULL;
	static constexpr uint64 GMontageRngZeroFallback = 0x13579bdf13579bdfULL;

	static uint64 MontageSplitMixFinalize(uint64 H)
	{
		H = (H ^ (H >> 30)) * GMontageSplitMixC1;
		H = (H ^ (H >> 27)) * GMontageSplitMixC2;
		return H ^ (H >> 31);
	}

	static int32 MontageHashCombine(int32 A, int32 B)
	{
		const uint64 UA = static_cast<uint64>(static_cast<uint32>(A));
		const uint64 UB = static_cast<uint64>(static_cast<uint32>(B));
		const uint64 H = MontageSplitMixFinalize((UA << 32) | UB);
		return static_cast<int32>(static_cast<uint32>(H));
	}

	struct FMontageSplitMixRng
	{
		uint64 State;

		explicit FMontageSplitMixRng(int32 Seed)
		{
			State = static_cast<uint64>(static_cast<uint32>(Seed)) * GMontageSplitMixGamma;
			if (State == 0)
			{
				State = GMontageRngZeroFallback;
			}
		}

		uint64 NextU64()
		{
			State = State + GMontageSplitMixGamma;
			return MontageSplitMixFinalize(State);
		}

		double NextUnit()
		{
			const uint64 V = NextU64() >> 11;
			return static_cast<double>(V) * (1.0 / 9007199254740992.0);
		}
	};

	static int32 StableActorSeedFromPath(const FString& ActorPath)
	{
		uint64 H = 14695981039346656037ULL;
		constexpr uint64 Prime = 1099511628211ULL;
		for (int32 I = 0; I < ActorPath.Len(); ++I)
		{
			const uint32 C = static_cast<uint32>(ActorPath[I]);
			H ^= (C & 0xffU); H *= Prime;
			H ^= ((C >> 8) & 0xffU); H *= Prime;
			H ^= ((C >> 16) & 0xffU); H *= Prime;
			H ^= ((C >> 24) & 0xffU); H *= Prime;
		}
		return static_cast<int32>(static_cast<uint32>(MontageSplitMixFinalize(H)));
	}

	static int32 MontageWeightedPick(const TArray<double>& Cumulative, double Target)
	{
		for (int32 I = 0; I < Cumulative.Num(); ++I)
		{
			if (Target <= Cumulative[I])
			{
				return I;
			}
		}
		return Cumulative.Num() > 0 ? Cumulative.Num() - 1 : 0;
	}

	static bool TryReadMontageFloat(const FJsonObject& Object, const TCHAR* FieldName, float& InOut)
	{
		double D = 0.0;
		if (!Object.TryGetNumberField(FieldName, D))
		{
			return true;
		}

		const float F = static_cast<float>(D);
		if (!FMath::IsFinite(F))
		{
			return false;
		}

		InOut = F;
		return true;
	}

	static void AddMontagePoolIssue(TArray<TSharedPtr<FJsonValue>>& Issues, int32 RawIndex, const FString& Reason)
	{
		TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetNumberField(TEXT("raw_index"), RawIndex);
		Issue->SetStringField(TEXT("reason"), Reason);
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	static void CollectCandidateActors(AActor* Root, const FString& Mode, TArray<AActor*>& Out)
	{
		if (!Root) return;
		const bool bAttached = (Mode.Equals(TEXT("attached"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("all"), ESearchCase::IgnoreCase));
		const bool bChildren = (Mode.Equals(TEXT("children"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("all"), ESearchCase::IgnoreCase) || Mode.IsEmpty());

		if (bChildren)
		{
			TArray<AActor*> Attached;
			Root->GetAttachedActors(Attached, /*bResetArray*/true, /*bRecursivelyIncludeAttachedActors*/true);
			for (AActor* A : Attached) if (A) Out.AddUnique(A);
		}
		if (bAttached)
		{
			// Actors parented by SpawnActor with Owner=Root — common for PCG ActorSpawner.
			for (TActorIterator<AActor> It(Root->GetWorld()); It; ++It)
			{
				AActor* A = *It;
				if (A && A->GetOwner() == Root) Out.AddUnique(A);
			}
		}
		// Always include the root itself if it has a SkeletalMeshComponent.
		Out.AddUnique(Root);
	}

	static bool ValidateMontagePoolForDecorate(
		const FSololmcpToolExecutionContext& Context,
		const TArray<TSharedPtr<FJsonValue>>& PoolJson,
		TArray<FMontagePoolEntry>& OutPool,
		TArray<double>& OutCumulative,
		double& OutTotalWeight,
		TMap<FString, UAnimMontage*>& OutMontageCache,
		TSharedRef<FJsonObject>& OutValidation,
		FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> SkippedEntries;
		TArray<TSharedPtr<FJsonValue>> AssetErrors;
		OutPool.Reset();
		OutCumulative.Reset();
		OutTotalWeight = 0.0;

		for (int32 RawIndex = 0; RawIndex < PoolJson.Num(); ++RawIndex)
		{
			const TSharedPtr<FJsonValue>& V = PoolJson[RawIndex];
			if (!V.IsValid() || V->Type != EJson::Object)
			{
				AddMontagePoolIssue(SkippedEntries, RawIndex, TEXT("entry is not an object"));
				continue;
			}

			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid())
			{
				AddMontagePoolIssue(SkippedEntries, RawIndex, TEXT("entry object is invalid"));
				continue;
			}

			FMontagePoolEntry E;
			E.RawIndex = RawIndex;
			O->TryGetStringField(TEXT("path"), E.Path);
			if (E.Path.IsEmpty())
			{
				AddMontagePoolIssue(SkippedEntries, RawIndex, TEXT("empty path"));
				continue;
			}

			if (!TryReadMontageFloat(*O, TEXT("weight"), E.Weight) || E.Weight <= 0.0f)
			{
				AddMontagePoolIssue(SkippedEntries, RawIndex,
					FString::Printf(TEXT("non-positive or non-finite weight: %f"), E.Weight));
				continue;
			}
			if (!TryReadMontageFloat(*O, TEXT("play_rate_min"), E.PlayRateMin))
			{
				AddMontagePoolIssue(SkippedEntries, RawIndex, TEXT("non-finite play_rate_min"));
				continue;
			}
			if (!TryReadMontageFloat(*O, TEXT("play_rate_max"), E.PlayRateMax))
			{
				AddMontagePoolIssue(SkippedEntries, RawIndex, TEXT("non-finite play_rate_max"));
				continue;
			}

			OutTotalWeight += static_cast<double>(E.Weight);
			OutCumulative.Add(OutTotalWeight);
			E.PoolIndex = OutPool.Num();
			OutPool.Add(MoveTemp(E));
		}

		OutValidation->SetStringField(TEXT("sampler_version"), GMontageSamplerVersion);
		OutValidation->SetNumberField(TEXT("pool_size_in"), PoolJson.Num());
		OutValidation->SetNumberField(TEXT("pool_size_valid"), OutPool.Num());
		OutValidation->SetNumberField(TEXT("total_weight"), OutTotalWeight);
		OutValidation->SetArrayField(TEXT("skipped_pool_entries"), SkippedEntries);

		if (OutPool.Num() == 0 || OutTotalWeight <= 0.0)
		{
			OutValidation->SetBoolField(TEXT("ok"), false);
			OutValidation->SetArrayField(TEXT("asset_errors"), AssetErrors);
			OutError = TEXT("montage_pool has no valid positive-weight entries");
			return false;
		}

		for (const FMontagePoolEntry& E : OutPool)
		{
			if (OutMontageCache.Contains(E.Path))
			{
				continue;
			}

			FString LoadErr;
			UObject* Asset = Context.Services.LoadAsset(E.Path, LoadErr);
			UAnimMontage* Montage = Cast<UAnimMontage>(Asset);
			OutMontageCache.Add(E.Path, Montage);
			if (!Montage)
			{
				const FString Detail = LoadErr.IsEmpty()
					? FString::Printf(TEXT("asset is not an AnimMontage: %s"), *E.Path)
					: FString::Printf(TEXT("%s: %s"), *E.Path, *LoadErr);
				AddMontagePoolIssue(AssetErrors, E.RawIndex, Detail);
			}
		}

		OutValidation->SetArrayField(TEXT("asset_errors"), AssetErrors);
		OutValidation->SetBoolField(TEXT("ok"), AssetErrors.Num() == 0);
		if (AssetErrors.Num() > 0)
		{
			OutError = TEXT("montage_pool contains entries that failed AnimMontage load/cast validation");
			return false;
		}

		return true;
	}

	static constexpr TCHAR GMontageDurableTagPrefix[] = TEXT("SOMO.PCG.Montage.");

	static void RemoveMontageDurableTags(TArray<FName>& Tags)
	{
		for (int32 I = Tags.Num() - 1; I >= 0; --I)
		{
			if (Tags[I].ToString().StartsWith(GMontageDurableTagPrefix, ESearchCase::CaseSensitive))
			{
				Tags.RemoveAtSwap(I, 1, SOMOLMCP_NO_SHRINK);
			}
		}
	}

	static TArray<FName> BuildMontageDurableTags(
		const FString& MontagePath,
		float PlayRate,
		int32 CombinedSeed,
		int32 PoolIndex,
		int32 RawPoolIndex)
	{
		TArray<FName> Tags;
		Tags.Reserve(7);
		Tags.Add(FName(TEXT("SOMO.PCG.Montage.Assigned")));
		Tags.Add(FName(*FString::Printf(TEXT("SOMO.PCG.Montage.Path=%s"), *MontagePath)));
		Tags.Add(FName(*FString::Printf(TEXT("SOMO.PCG.Montage.PlayRate=%.6f"), PlayRate)));
		Tags.Add(FName(*FString::Printf(TEXT("SOMO.PCG.Montage.CombinedSeed=%d"), CombinedSeed)));
		Tags.Add(FName(*FString::Printf(TEXT("SOMO.PCG.Montage.PoolIndex=%d"), PoolIndex)));
		Tags.Add(FName(*FString::Printf(TEXT("SOMO.PCG.Montage.RawPoolIndex=%d"), RawPoolIndex)));
		Tags.Add(FName(*FString::Printf(TEXT("SOMO.PCG.Montage.Sampler=%s"), GMontageSamplerVersion)));
		return Tags;
	}

	static void ReplaceMontageDurableTags(TArray<FName>& TargetTags, const TArray<FName>& NewTags)
	{
		RemoveMontageDurableTags(TargetTags);
		for (const FName& Tag : NewTags)
		{
			TargetTags.AddUnique(Tag);
		}
	}

	static TSharedRef<FJsonObject> PersistMontageDurableBinding(
		AActor* Actor,
		USkeletalMeshComponent* SkelComp,
		const FString& MontagePath,
		float PlayRate,
		int32 CombinedSeed,
		int32 PoolIndex,
		int32 RawPoolIndex)
	{
		TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Targets;
		TArray<TSharedPtr<FJsonValue>> Fields;
		const TArray<FName> Tags = BuildMontageDurableTags(MontagePath, PlayRate, CombinedSeed, PoolIndex, RawPoolIndex);

		auto AddTarget = [&Targets](const TCHAR* TargetType, UObject* Object, bool bModified, bool bPackageDirty)
		{
			TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
			Target->SetStringField(TEXT("target"), TargetType);
			Target->SetStringField(TEXT("path"), Object ? Object->GetPathName() : FString());
			Target->SetBoolField(TEXT("modified"), bModified);
			Target->SetBoolField(TEXT("package_dirty"), bPackageDirty);
			Targets.Add(MakeShared<FJsonValueObject>(Target));
		};

		auto AddField = [&Fields](const TCHAR* Key, const FString& Value)
		{
			TSharedRef<FJsonObject> Field = MakeShared<FJsonObject>();
			Field->SetStringField(TEXT("key"), Key);
			Field->SetStringField(TEXT("value"), Value);
			Fields.Add(MakeShared<FJsonValueObject>(Field));
		};

		bool bActorModified = false;
		bool bActorDirty = false;
		if (Actor)
		{
			bActorModified = Actor->Modify();
			ReplaceMontageDurableTags(Actor->Tags, Tags);
			bActorDirty = Actor->MarkPackageDirty();
			AddTarget(TEXT("actor_tags"), Actor, bActorModified, bActorDirty);
		}

		bool bComponentModified = false;
		bool bComponentDirty = false;
		if (SkelComp)
		{
			bComponentModified = SkelComp->Modify();
			ReplaceMontageDurableTags(SkelComp->ComponentTags, Tags);
			bComponentDirty = SkelComp->MarkPackageDirty();
			AddTarget(TEXT("skeletal_component_tags"), SkelComp, bComponentModified, bComponentDirty);
		}

		AddField(TEXT("SOMO.PCG.Montage.Assigned"), TEXT("true"));
		AddField(TEXT("SOMO.PCG.Montage.Path"), MontagePath);
		AddField(TEXT("SOMO.PCG.Montage.PlayRate"), FString::Printf(TEXT("%.6f"), PlayRate));
		AddField(TEXT("SOMO.PCG.Montage.CombinedSeed"), FString::FromInt(CombinedSeed));
		AddField(TEXT("SOMO.PCG.Montage.PoolIndex"), FString::FromInt(PoolIndex));
		AddField(TEXT("SOMO.PCG.Montage.RawPoolIndex"), FString::FromInt(RawPoolIndex));
		AddField(TEXT("SOMO.PCG.Montage.Sampler"), GMontageSamplerVersion);

		Binding->SetBoolField(TEXT("ok"), Actor != nullptr || SkelComp != nullptr);
		Binding->SetStringField(TEXT("method"), TEXT("actor_tags_and_skeletal_component_tags"));
		Binding->SetStringField(TEXT("tag_prefix"), GMontageDurableTagPrefix);
		Binding->SetArrayField(TEXT("targets"), Targets);
		Binding->SetArrayField(TEXT("fields"), Fields);
		return Binding;
	}

	static TSharedPtr<FJsonValue> MakeStringJsonValue(const FString& Value)
	{
		return MakeShared<FJsonValueString>(Value);
	}

	static TArray<TSharedPtr<FJsonValue>> NamesToJsonArray(const TArray<FName>& Names)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Names.Num());
		for (const FName& Name : Names)
		{
			Out.Add(MakeStringJsonValue(Name.ToString()));
		}
		return Out;
	}

	static FString ObjectPathOrEmpty(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	static FString AssetPathOrEmpty(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	static TArray<TSharedPtr<FJsonValue>> CollectTagsWithPrefix(const TArray<FName>& Tags, const FString& Prefix)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FName& Tag : Tags)
		{
			const FString S = Tag.ToString();
			if (S.StartsWith(Prefix, ESearchCase::CaseSensitive))
			{
				Out.Add(MakeStringJsonValue(S));
			}
		}
		return Out;
	}

	static void MergeMontageTagFields(const TArray<FName>& Tags, TSharedRef<FJsonObject>& Fields)
	{
		for (const FName& TagName : Tags)
		{
			const FString Tag = TagName.ToString();
			if (!Tag.StartsWith(GMontageDurableTagPrefix, ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (Tag.Equals(TEXT("SOMO.PCG.Montage.Assigned"), ESearchCase::CaseSensitive))
			{
				Fields->SetBoolField(TEXT("assigned"), true);
				continue;
			}
			FString Key;
			FString Value;
			if (!Tag.Split(TEXT("="), &Key, &Value))
			{
				continue;
			}
			if (Key.Equals(TEXT("SOMO.PCG.Montage.Path"), ESearchCase::CaseSensitive))
			{
				Fields->SetStringField(TEXT("path"), Value);
			}
			else if (Key.Equals(TEXT("SOMO.PCG.Montage.PlayRate"), ESearchCase::CaseSensitive))
			{
				Fields->SetStringField(TEXT("play_rate"), Value);
			}
			else if (Key.Equals(TEXT("SOMO.PCG.Montage.CombinedSeed"), ESearchCase::CaseSensitive))
			{
				Fields->SetStringField(TEXT("combined_seed"), Value);
			}
			else if (Key.Equals(TEXT("SOMO.PCG.Montage.PoolIndex"), ESearchCase::CaseSensitive))
			{
				Fields->SetStringField(TEXT("pool_index"), Value);
			}
			else if (Key.Equals(TEXT("SOMO.PCG.Montage.RawPoolIndex"), ESearchCase::CaseSensitive))
			{
				Fields->SetStringField(TEXT("raw_pool_index"), Value);
			}
			else if (Key.Equals(TEXT("SOMO.PCG.Montage.Sampler"), ESearchCase::CaseSensitive))
			{
				Fields->SetStringField(TEXT("sampler"), Value);
			}
		}
	}

	static TSharedRef<FJsonObject> BuildDurableMontageTagReport(AActor* Actor, const TArray<USkeletalMeshComponent*>& SkelComps)
	{
		TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ActorTags;
		TArray<TSharedPtr<FJsonValue>> ComponentReports;

		if (Actor)
		{
			ActorTags = CollectTagsWithPrefix(Actor->Tags, GMontageDurableTagPrefix);
			MergeMontageTagFields(Actor->Tags, Fields);
		}

		for (USkeletalMeshComponent* Comp : SkelComps)
		{
			if (!Comp)
			{
				continue;
			}
			TArray<TSharedPtr<FJsonValue>> Tags = CollectTagsWithPrefix(Comp->ComponentTags, GMontageDurableTagPrefix);
			MergeMontageTagFields(Comp->ComponentTags, Fields);
			if (Tags.Num() == 0)
			{
				continue;
			}
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("component_path"), Comp->GetPathName());
			C->SetArrayField(TEXT("tags"), Tags);
			ComponentReports.Add(MakeShared<FJsonValueObject>(C));
		}

		bool bAssigned = false;
		Fields->TryGetBoolField(TEXT("assigned"), bAssigned);
		Report->SetBoolField(TEXT("assigned"), bAssigned);
		Report->SetStringField(TEXT("tag_prefix"), GMontageDurableTagPrefix);
		Report->SetArrayField(TEXT("actor_tags"), ActorTags);
		Report->SetArrayField(TEXT("component_tags"), ComponentReports);
		Report->SetObjectField(TEXT("fields"), Fields);
		return Report;
	}

	static TSharedRef<FJsonObject> BuildMontageAssetInfo(
		const FMontagePoolEntry& Entry,
		UAnimMontage* Montage,
		const TArray<USkeleton*>& ExpectedSkeletons,
		TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetNumberField(TEXT("raw_index"), Entry.RawIndex);
		Info->SetNumberField(TEXT("pool_index"), Entry.PoolIndex);
		Info->SetStringField(TEXT("path"), Entry.Path);
		Info->SetNumberField(TEXT("weight"), Entry.Weight);
		Info->SetNumberField(TEXT("play_rate_min"), Entry.PlayRateMin);
		Info->SetNumberField(TEXT("play_rate_max"), Entry.PlayRateMax);
		Info->SetBoolField(TEXT("loaded"), Montage != nullptr);

		if (Entry.PlayRateMin <= 0.0f || Entry.PlayRateMax <= 0.0f)
		{
			AddMontagePoolIssue(Warnings, Entry.RawIndex, TEXT("play_rate_min/play_rate_max should be positive; decorate clamps to >= 0.01"));
		}
		if (Entry.PlayRateMax < Entry.PlayRateMin)
		{
			AddMontagePoolIssue(Warnings, Entry.RawIndex, TEXT("play_rate_max is lower than play_rate_min; decorate collapses to play_rate_min"));
		}

		if (!Montage)
		{
			return Info;
		}

		USkeleton* Skeleton = Montage->GetSkeleton();
		Info->SetStringField(TEXT("asset_class"), Montage->GetClass()->GetName());
		Info->SetStringField(TEXT("skeleton_path"), AssetPathOrEmpty(Skeleton));
		Info->SetNumberField(TEXT("play_length"), Montage->GetPlayLength());
		Info->SetNumberField(TEXT("rate_scale"), Montage->RateScale);

		TArray<TSharedPtr<FJsonValue>> Slots;
		for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
		{
			Slots.Add(MakeStringJsonValue(Track.SlotName.ToString()));
		}
		Info->SetArrayField(TEXT("slots"), Slots);

		if (Slots.Num() == 0)
		{
			AddMontagePoolIssue(Warnings, Entry.RawIndex, TEXT("montage has no SlotAnimTracks"));
		}
		if (!Skeleton)
		{
			AddMontagePoolIssue(Warnings, Entry.RawIndex, TEXT("montage has no skeleton"));
		}
		else if (ExpectedSkeletons.Num() > 0 && !ExpectedSkeletons.Contains(Skeleton))
		{
			AddMontagePoolIssue(Warnings, Entry.RawIndex,
				FString::Printf(TEXT("montage skeleton '%s' does not match any expected target skeleton"), *Skeleton->GetPathName()));
		}

		return Info;
	}

	static void CollectIndexActors(UWorld* World, AActor* Root, const FString& Scope, TArray<AActor*>& Out)
	{
		const FString NormalizedScope = Scope.IsEmpty() ? TEXT("children") : Scope;
		if (NormalizedScope.Equals(TEXT("all"), ESearchCase::IgnoreCase) && !Root)
		{
			if (!World)
			{
				return;
			}
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (*It)
				{
					Out.AddUnique(*It);
				}
			}
			return;
		}

		if (!Root)
		{
			return;
		}
		if (NormalizedScope.Equals(TEXT("actor"), ESearchCase::IgnoreCase))
		{
			Out.AddUnique(Root);
			return;
		}

		CollectCandidateActors(Root, NormalizedScope, Out);
	}

	static bool TagsContainPcgHint(const TArray<FName>& Tags)
	{
		for (const FName& TagName : Tags)
		{
			if (TagName.ToString().Contains(TEXT("PCG"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static TSharedRef<FJsonObject> BuildComponentProvenanceHints(AActor* Actor, bool bHasPcgComponent)
	{
		TSharedRef<FJsonObject> Hints = MakeShared<FJsonObject>();
		Hints->SetBoolField(TEXT("actor_has_pcg_component"), bHasPcgComponent);
		Hints->SetBoolField(TEXT("actor_tags_contain_pcg"), Actor ? TagsContainPcgHint(Actor->Tags) : false);
		Hints->SetStringField(TEXT("owner_path"), ObjectPathOrEmpty(Actor ? Actor->GetOwner() : nullptr));
		Hints->SetStringField(TEXT("attach_parent_actor_path"),
			(Actor && Actor->GetAttachParentActor()) ? Actor->GetAttachParentActor()->GetPathName() : FString());
		return Hints;
	}

	static TSharedRef<FJsonObject> BuildGeneratedActorCleanupEvidence(
		AActor* Actor,
		AActor* ScopeRoot,
		bool bHasPcgComponent,
		int32 InstanceCount)
	{
		const bool bOwnedByScope = Actor && ScopeRoot && Actor->GetOwner() == ScopeRoot;
		const bool bAttachedToScope = Actor && ScopeRoot && Actor->GetAttachParentActor() == ScopeRoot;
		const bool bTaggedPcg = Actor ? TagsContainPcgHint(Actor->Tags) : false;
		const bool bCleanupCandidate = Actor && Actor != ScopeRoot && (bOwnedByScope || bAttachedToScope || bTaggedPcg || bHasPcgComponent || InstanceCount > 0);

		TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(TEXT("schema"), TEXT("somol.pcg.generated_actor_cleanup.v1"));
		Evidence->SetBoolField(TEXT("cleanup_performed"), false);
		Evidence->SetBoolField(TEXT("cleanup_candidate"), bCleanupCandidate);
		Evidence->SetBoolField(TEXT("owned_by_scope_root"), bOwnedByScope);
		Evidence->SetBoolField(TEXT("attached_to_scope_root"), bAttachedToScope);
		Evidence->SetBoolField(TEXT("actor_tags_contain_pcg"), bTaggedPcg);
		Evidence->SetBoolField(TEXT("has_pcg_component"), bHasPcgComponent);
		Evidence->SetNumberField(TEXT("instance_count"), InstanceCount);
		Evidence->SetStringField(TEXT("scope_root_path"), ObjectPathOrEmpty(ScopeRoot));
		Evidence->SetStringField(TEXT("note"), TEXT("Index only: no actors/components are destroyed. Use these fields as evidence before an explicit cleanup mutation."));
		return Evidence;
	}

	static constexpr TCHAR GPcgTileBatchSchema[] = TEXT("somol.pcg.tile_batch.v1");
	static constexpr TCHAR GPcgGeneratedProvenanceSchema[] = TEXT("somol.pcg.generated_actor_provenance.v1");
	static constexpr TCHAR GPcgProvenanceTagPrefix[] = TEXT("SOMO.PCG.");

	struct FPcgTileDescriptor
	{
		FString TileId;
		int32 Index = INDEX_NONE;
		int32 Col = INDEX_NONE;
		int32 Row = INDEX_NONE;
		bool bHasCoord = false;
		bool bHasBoundsM = false;
		double MinX_M = 0.0;
		double MinY_M = 0.0;
		double MaxX_M = 0.0;
		double MaxY_M = 0.0;
		TArray<FString> ActorHints;
	};

	struct FPcgIndexScanRequest
	{
		FString Scope = TEXT("children");
		FString ActorId;
		FString NamePattern;
		bool bIncludeEmpty = false;
		int32 MaxActors = 1000;
		AActor* Root = nullptr;
		UWorld* World = nullptr;
		TArray<AActor*> Candidates;
	};

	struct FPcgGeneratedProvenanceFields
	{
		FString TileId;
		FString GenerationId;
		FString SourceComponentPath;
		FString SourceGraphPath;
		FString GraphHash;
		int32 Seed = 0;
		bool bHasSeed = false;
		bool bTileIdInferred = false;
		bool bGenerationIdInferred = false;
		bool bSourceInferred = false;
		bool bGraphHashInferred = false;
		TArray<FString> EvidenceSources;
	};

	static TSharedRef<FJsonObject> BuildPcgTileCapPolicy(const FString& Mode, bool bFailClosed = false)
	{
		TSharedRef<FJsonObject> Policy = MakeShared<FJsonObject>();
		Policy->SetStringField(TEXT("schema"), PcgExecutionSafety::PcgTileCapSchema);
		Policy->SetNumberField(TEXT("max_tiles_per_generate"), PcgExecutionSafety::PcgMaxTilesPerGenerate);
		Policy->SetBoolField(TEXT("require_tile_evidence"), true);
		Policy->SetBoolField(TEXT("fail_closed"), bFailClosed);
		Policy->SetBoolField(TEXT("native_tile_mask_available"), false);
		Policy->SetStringField(TEXT("enforcement_mode"), Mode);
		Policy->SetStringField(TEXT("note"), TEXT("Tile-batch tools provide deterministic planning/status/provenance receipts; generation still routes through guarded actor/component PCG calls."));
		return Policy;
	}

	static bool TryParseIntStrict(const FString& Text, int32& Out)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.IsNumeric())
		{
			return false;
		}
		Out = FCString::Atoi(*Trimmed);
		return true;
	}

	static bool TryReadJsonIntFieldLoose(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, int32& Out)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		double D = 0.0;
		if (Obj->TryGetNumberField(FieldName, D))
		{
			const int32 I = FMath::RoundToInt(D);
			if (FMath::IsNearlyEqual(D, static_cast<double>(I), KINDA_SMALL_NUMBER))
			{
				Out = I;
				return true;
			}
		}
		FString S;
		if (Obj->TryGetStringField(FieldName, S))
		{
			return TryParseIntStrict(S, Out);
		}
		return false;
	}

	static bool TryReadJsonNumberFieldLoose(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, double& Out)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		double D = 0.0;
		if (Obj->TryGetNumberField(FieldName, D) && FMath::IsFinite(D))
		{
			Out = D;
			return true;
		}
		FString S;
		if (Obj->TryGetStringField(FieldName, S) && !S.IsEmpty())
		{
			const double Parsed = FCString::Atod(*S);
			if (FMath::IsFinite(Parsed))
			{
				Out = Parsed;
				return true;
			}
		}
		return false;
	}

	static uint64 StablePcgHash64(const FString& Text)
	{
		uint64 H = 14695981039346656037ULL;
		constexpr uint64 Prime = 1099511628211ULL;
		for (int32 I = 0; I < Text.Len(); ++I)
		{
			const uint32 C = static_cast<uint32>(Text[I]);
			H ^= (C & 0xffU); H *= Prime;
			H ^= ((C >> 8) & 0xffU); H *= Prime;
			H ^= ((C >> 16) & 0xffU); H *= Prime;
			H ^= ((C >> 24) & 0xffU); H *= Prime;
		}
		return MontageSplitMixFinalize(H);
	}

	static FString StablePcgHashHex(const FString& Text)
	{
		const uint64 H = StablePcgHash64(Text);
		return FString::Printf(TEXT("%08x%08x"), static_cast<uint32>(H >> 32), static_cast<uint32>(H));
	}

	static int32 StablePcgSeedFromParts(int32 BaseSeed, const FString& TileId, const FString& GenerationId, const FString& GraphPath, const FString& Salt)
	{
		const FString Key = FString::Printf(TEXT("%d|%s|%s|%s|%s"), BaseSeed, *TileId, *GenerationId, *GraphPath, *Salt);
		return static_cast<int32>(static_cast<uint32>(StablePcgHash64(Key)));
	}

	static FString ResolvePcgGenerationId(const TSharedRef<FJsonObject>& Arguments, const FString& ActorId, const FString& GraphPath)
	{
		FString GenerationId;
		if (Arguments->TryGetStringField(TEXT("generation_id"), GenerationId) && !GenerationId.TrimStartAndEnd().IsEmpty())
		{
			return GenerationId.TrimStartAndEnd();
		}
		FString ClientRequestId;
		if (Arguments->TryGetStringField(TEXT("client_request_id"), ClientRequestId) && !ClientRequestId.TrimStartAndEnd().IsEmpty())
		{
			return ClientRequestId.TrimStartAndEnd();
		}
		return FString::Printf(TEXT("gen_%s"), *StablePcgHashHex(ActorId + TEXT("|") + GraphPath).Left(12));
	}

	static FPcgTileDescriptor MakeCoordTileDescriptor(int32 Col, int32 Row, double TileSizeM, double OriginX_M, double OriginY_M)
	{
		FPcgTileDescriptor D;
		D.Col = Col;
		D.Row = Row;
		D.bHasCoord = true;
		D.TileId = FString::Printf(TEXT("coord:%d,%d"), Col, Row);
		D.bHasBoundsM = true;
		D.MinX_M = OriginX_M + static_cast<double>(Col) * TileSizeM;
		D.MinY_M = OriginY_M + static_cast<double>(Row) * TileSizeM;
		D.MaxX_M = D.MinX_M + TileSizeM;
		D.MaxY_M = D.MinY_M + TileSizeM;
		AddUniqueString(D.ActorHints, FString::Printf(TEXT("PCGV_Fill_%d_%d"), Col, Row));
		AddUniqueString(D.ActorHints, FString::Printf(TEXT("%d_%d"), Col, Row));
		return D;
	}

	static void ReadTileBoundsFromObject(const TSharedPtr<FJsonObject>& Obj, FPcgTileDescriptor& Out)
	{
		if (!Obj.IsValid())
		{
			return;
		}
		double MinX = 0.0;
		double MinY = 0.0;
		double MaxX = 0.0;
		double MaxY = 0.0;
		const bool bDirect =
			TryReadJsonNumberFieldLoose(Obj, TEXT("min_x_m"), MinX) &&
			TryReadJsonNumberFieldLoose(Obj, TEXT("min_y_m"), MinY) &&
			TryReadJsonNumberFieldLoose(Obj, TEXT("max_x_m"), MaxX) &&
			TryReadJsonNumberFieldLoose(Obj, TEXT("max_y_m"), MaxY);
		if (bDirect)
		{
			Out.bHasBoundsM = true;
			Out.MinX_M = FMath::Min(MinX, MaxX);
			Out.MinY_M = FMath::Min(MinY, MaxY);
			Out.MaxX_M = FMath::Max(MinX, MaxX);
			Out.MaxY_M = FMath::Max(MinY, MaxY);
			return;
		}
		const TSharedPtr<FJsonObject>* BoundsObj = nullptr;
		if ((Obj->TryGetObjectField(TEXT("bounds"), BoundsObj) || Obj->TryGetObjectField(TEXT("aoi_bounds"), BoundsObj)) &&
			BoundsObj && BoundsObj->IsValid())
		{
			ReadTileBoundsFromObject(*BoundsObj, Out);
		}
	}

	static bool ReadPcgTileDescriptor(const TSharedPtr<FJsonValue>& Value, int32 FallbackIndex, FPcgTileDescriptor& Out, FString& OutError)
	{
		Out = FPcgTileDescriptor();
		if (!Value.IsValid() || Value->IsNull())
		{
			OutError = TEXT("tile descriptor is null");
			return false;
		}

		if (Value->Type == EJson::Number)
		{
			int32 Index = 0;
			if (!TryJsonNumberAsInt(Value, Index))
			{
				OutError = TEXT("numeric tile descriptor must be an integer");
				return false;
			}
			Out.Index = Index;
			Out.TileId = FString::Printf(TEXT("index:%d"), Index);
			AddUniqueString(Out.ActorHints, FString::FromInt(Index));
			return true;
		}

		if (Value->Type == EJson::String)
		{
			Out.TileId = Value->AsString().TrimStartAndEnd();
			if (Out.TileId.IsEmpty())
			{
				OutError = TEXT("string tile descriptor is empty");
				return false;
			}
			AddUniqueString(Out.ActorHints, Out.TileId);
			return true;
		}

		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
			if (Arr.Num() < 2)
			{
				OutError = TEXT("array tile descriptor must contain [col,row]");
				return false;
			}
			int32 Col = 0;
			int32 Row = 0;
			if (!TryJsonNumberAsInt(Arr[0], Col) || !TryJsonNumberAsInt(Arr[1], Row))
			{
				OutError = TEXT("array tile descriptor col/row must be integers");
				return false;
			}
			Out = MakeCoordTileDescriptor(Col, Row, 256.0, 0.0, 0.0);
			Out.bHasBoundsM = false;
			return true;
		}

		if (Value->Type != EJson::Object)
		{
			OutError = TEXT("tile descriptor must be a string, integer, [col,row], or object");
			return false;
		}

		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		if (!Obj.IsValid())
		{
			OutError = TEXT("tile descriptor object is invalid");
			return false;
		}

		FString Id;
		if (Obj->TryGetStringField(TEXT("tile_id"), Id) || Obj->TryGetStringField(TEXT("id"), Id))
		{
			Out.TileId = Id.TrimStartAndEnd();
			AddUniqueString(Out.ActorHints, Out.TileId);
		}

		int32 Col = 0;
		int32 Row = 0;
		if (TryReadJsonIntFieldLoose(Obj, TEXT("col"), Col) && TryReadJsonIntFieldLoose(Obj, TEXT("row"), Row))
		{
			Out.Col = Col;
			Out.Row = Row;
			Out.bHasCoord = true;
			if (Out.TileId.IsEmpty())
			{
				Out.TileId = FString::Printf(TEXT("coord:%d,%d"), Col, Row);
			}
			AddUniqueString(Out.ActorHints, FString::Printf(TEXT("PCGV_Fill_%d_%d"), Col, Row));
			AddUniqueString(Out.ActorHints, FString::Printf(TEXT("%d_%d"), Col, Row));
		}

		int32 Index = INDEX_NONE;
		if (TryReadJsonIntFieldLoose(Obj, TEXT("tile_index"), Index) || TryReadJsonIntFieldLoose(Obj, TEXT("index"), Index))
		{
			Out.Index = Index;
			if (Out.TileId.IsEmpty())
			{
				Out.TileId = FString::Printf(TEXT("index:%d"), Index);
			}
			AddUniqueString(Out.ActorHints, FString::FromInt(Index));
		}

		FString ActorHint;
		if (Obj->TryGetStringField(TEXT("actor"), ActorHint) ||
			Obj->TryGetStringField(TEXT("actor_name"), ActorHint) ||
			Obj->TryGetStringField(TEXT("actor_path"), ActorHint) ||
			Obj->TryGetStringField(TEXT("volume_actor"), ActorHint))
		{
			AddUniqueString(Out.ActorHints, ActorHint);
		}
		ReadTileBoundsFromObject(Obj, Out);

		if (Out.TileId.IsEmpty())
		{
			Out.TileId = FString::Printf(TEXT("tile:%d"), FallbackIndex);
		}
		return true;
	}

	static bool ReadAoiBoundsFromArguments(const TSharedRef<FJsonObject>& Arguments, double& MinX, double& MinY, double& MaxX, double& MaxY)
	{
		const TSharedPtr<FJsonObject>* BoundsObj = nullptr;
		if (Arguments->TryGetObjectField(TEXT("aoi_bounds"), BoundsObj) && BoundsObj && BoundsObj->IsValid())
		{
			return
				TryReadJsonNumberFieldLoose(*BoundsObj, TEXT("min_x_m"), MinX) &&
				TryReadJsonNumberFieldLoose(*BoundsObj, TEXT("min_y_m"), MinY) &&
				TryReadJsonNumberFieldLoose(*BoundsObj, TEXT("max_x_m"), MaxX) &&
				TryReadJsonNumberFieldLoose(*BoundsObj, TEXT("max_y_m"), MaxY);
		}
		return
			Arguments->TryGetNumberField(TEXT("aoi_min_x_m"), MinX) &&
			Arguments->TryGetNumberField(TEXT("aoi_min_y_m"), MinY) &&
			Arguments->TryGetNumberField(TEXT("aoi_max_x_m"), MaxX) &&
			Arguments->TryGetNumberField(TEXT("aoi_max_y_m"), MaxY);
	}

	static bool ReadPcgTileDescriptorsFromArguments(
		const TSharedRef<FJsonObject>& Arguments,
		TArray<FPcgTileDescriptor>& OutTiles,
		FString& OutSource,
		bool& bOutTruncated,
		FString& OutError)
	{
		OutTiles.Reset();
		OutSource = TEXT("none");
		bOutTruncated = false;

		const TCHAR* ArrayFields[] = { TEXT("tiles_requested"), TEXT("tiles"), TEXT("allowed_tiles"), TEXT("tile_indices") };
		for (const TCHAR* FieldName : ArrayFields)
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (!Arguments->TryGetArrayField(FieldName, Arr) || !Arr)
			{
				continue;
			}
			OutSource = FieldName;
			for (int32 I = 0; I < Arr->Num(); ++I)
			{
				FPcgTileDescriptor Tile;
				if (!ReadPcgTileDescriptor((*Arr)[I], I, Tile, OutError))
				{
					OutError = FString::Printf(TEXT("%s[%d]: %s"), FieldName, I, *OutError);
					return false;
				}
				OutTiles.Add(Tile);
			}
			return true;
		}

		double MinX = 0.0;
		double MinY = 0.0;
		double MaxX = 0.0;
		double MaxY = 0.0;
		if (!ReadAoiBoundsFromArguments(Arguments, MinX, MinY, MaxX, MaxY))
		{
			return true;
		}

		double TileSizeM = 256.0;
		Arguments->TryGetNumberField(TEXT("tile_size_m"), TileSizeM);
		TileSizeM = FMath::Max(1.0, TileSizeM);
		double OriginX = 0.0;
		double OriginY = 0.0;
		Arguments->TryGetNumberField(TEXT("origin_x_m"), OriginX);
		Arguments->TryGetNumberField(TEXT("origin_y_m"), OriginY);

		const double LoX = FMath::Min(MinX, MaxX);
		const double LoY = FMath::Min(MinY, MaxY);
		const double HiX = FMath::Max(MinX, MaxX);
		const double HiY = FMath::Max(MinY, MaxY);
		const int32 MinCol = FMath::FloorToInt((LoX - OriginX) / TileSizeM);
		const int32 MinRow = FMath::FloorToInt((LoY - OriginY) / TileSizeM);
		const int32 MaxCol = FMath::CeilToInt((HiX - OriginX) / TileSizeM) - 1;
		const int32 MaxRow = FMath::CeilToInt((HiY - OriginY) / TileSizeM) - 1;
		if (MaxCol < MinCol || MaxRow < MinRow)
		{
			OutError = TEXT("AOI bounds resolve to an empty tile range");
			return false;
		}

		double MaxReturnedD = 4096.0;
		Arguments->TryGetNumberField(TEXT("max_tiles_returned"), MaxReturnedD);
		const int32 MaxReturned = FMath::Clamp(static_cast<int32>(MaxReturnedD), 1, 100000);
		OutSource = TEXT("aoi_bounds");
		for (int32 Row = MinRow; Row <= MaxRow; ++Row)
		{
			for (int32 Col = MinCol; Col <= MaxCol; ++Col)
			{
				if (OutTiles.Num() >= MaxReturned)
				{
					bOutTruncated = true;
					return true;
				}
				OutTiles.Add(MakeCoordTileDescriptor(Col, Row, TileSizeM, OriginX, OriginY));
			}
		}
		return true;
	}

	static TSharedRef<FJsonObject> BuildPcgTileDescriptorJson(
		const FPcgTileDescriptor& Tile,
		const FString& GenerationId,
		const FString& GraphPath,
		int32 BaseSeed,
		const FString& Salt)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("tile_id"), Tile.TileId);
		J->SetStringField(TEXT("generation_id"), GenerationId);
		J->SetNumberField(TEXT("seed"), StablePcgSeedFromParts(BaseSeed, Tile.TileId, GenerationId, GraphPath, Salt));
		if (Tile.Index != INDEX_NONE)
		{
			J->SetNumberField(TEXT("tile_index"), Tile.Index);
		}
		if (Tile.bHasCoord)
		{
			J->SetNumberField(TEXT("col"), Tile.Col);
			J->SetNumberField(TEXT("row"), Tile.Row);
		}
		if (Tile.bHasBoundsM)
		{
			TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
			Bounds->SetNumberField(TEXT("min_x_m"), Tile.MinX_M);
			Bounds->SetNumberField(TEXT("min_y_m"), Tile.MinY_M);
			Bounds->SetNumberField(TEXT("max_x_m"), Tile.MaxX_M);
			Bounds->SetNumberField(TEXT("max_y_m"), Tile.MaxY_M);
			J->SetObjectField(TEXT("bounds_m"), Bounds);
		}
		if (Tile.ActorHints.Num() > 0)
		{
			J->SetArrayField(TEXT("actor_identity_hints"), StringArrayToJson(Tile.ActorHints));
		}
		return J;
	}

	static bool ResolvePcgIndexScanRequest(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		FPcgIndexScanRequest& Out,
		FString& OutError)
	{
		Out = FPcgIndexScanRequest();
		Arguments->TryGetStringField(TEXT("scope"), Out.Scope);
		if (Out.Scope.IsEmpty())
		{
			Out.Scope = TEXT("children");
		}
		Arguments->TryGetStringField(TEXT("actor"), Out.ActorId);
		if (Out.ActorId.IsEmpty())
		{
			Arguments->TryGetStringField(TEXT("actor_label"), Out.ActorId);
		}
		if (!Out.ActorId.IsEmpty())
		{
			Out.Root = Context.Services.FindActorByLabelOrName(Out.ActorId, OutError);
			if (!Out.Root)
			{
				return false;
			}
		}
		else if (!Out.Scope.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("Missing required argument: actor (unless scope='all')");
			return false;
		}

		FString WorldError;
		Out.World = Out.Root ? Out.Root->GetWorld() : Context.Services.GetEditorWorld(WorldError);
		if (!Out.World)
		{
			OutError = WorldError.IsEmpty() ? TEXT("No editor world available") : WorldError;
			return false;
		}

		Arguments->TryGetStringField(TEXT("actor_name_pattern"), Out.NamePattern);
		Arguments->TryGetBoolField(TEXT("include_empty"), Out.bIncludeEmpty);
		double MaxActorsD = 1000.0;
		Arguments->TryGetNumberField(TEXT("max_actors"), MaxActorsD);
		Out.MaxActors = FMath::Clamp(static_cast<int32>(MaxActorsD), 1, 100000);
		CollectIndexActors(Out.World, Out.Root, Out.Scope, Out.Candidates);
		return true;
	}

	static bool ActorPassesPcgIndexFilters(AActor* Actor, const FPcgIndexScanRequest& Scan)
	{
		if (!Actor)
		{
			return false;
		}
		if (!Scan.NamePattern.IsEmpty() && !Actor->GetName().Contains(Scan.NamePattern, ESearchCase::IgnoreCase) &&
			!Actor->GetActorLabel().Contains(Scan.NamePattern, ESearchCase::IgnoreCase))
		{
			return false;
		}
		return true;
	}

	static FString FirstPcgComponentPath(AActor* Actor)
	{
		if (!Actor)
		{
			return FString();
		}
		TArray<UPCGComponent*> PcgComps;
		Actor->GetComponents<UPCGComponent>(PcgComps);
		for (UPCGComponent* Comp : PcgComps)
		{
			if (Comp)
			{
				return Comp->GetPathName();
			}
		}
		return FString();
	}

	static FString FirstPcgGraphPath(AActor* Actor)
	{
		if (!Actor)
		{
			return FString();
		}
		TArray<UPCGComponent*> PcgComps;
		Actor->GetComponents<UPCGComponent>(PcgComps);
		for (UPCGComponent* Comp : PcgComps)
		{
			if (!Comp)
			{
				continue;
			}
			const FString GraphPath = PcgExecutionSafety::ResolveComponentGraphPath(Comp);
			if (!GraphPath.IsEmpty())
			{
				return GraphPath;
			}
		}
		return FString();
	}

	static bool TryExtractPcgTagValue(const TArray<FName>& Tags, const FString& Key, FString& OutValue)
	{
		for (const FName& TagName : Tags)
		{
			const FString Tag = TagName.ToString();
			FString TagKey;
			FString TagValue;
			if (!Tag.Split(TEXT("="), &TagKey, &TagValue))
			{
				continue;
			}
			if (TagKey.Equals(Key, ESearchCase::IgnoreCase))
			{
				OutValue = TagValue.TrimStartAndEnd();
				return !OutValue.IsEmpty();
			}
		}
		return false;
	}

	static bool HasPcgTagKey(const TArray<FName>& Tags, const FString& Key)
	{
		FString Ignored;
		return TryExtractPcgTagValue(Tags, Key, Ignored);
	}

	static void MergePcgProvenanceTags(const TArray<FName>& Tags, const FString& Source, FPcgGeneratedProvenanceFields& InOut)
	{
		FString Value;
		if (InOut.TileId.IsEmpty() &&
			(TryExtractPcgTagValue(Tags, TEXT("SOMO.PCG.TileId"), Value) ||
			 TryExtractPcgTagValue(Tags, TEXT("SOMO.PCG.Tile"), Value)))
		{
			InOut.TileId = Value;
			AddUniqueString(InOut.EvidenceSources, Source + TEXT(":tile_id_tag"));
		}
		if (InOut.GenerationId.IsEmpty() &&
			(TryExtractPcgTagValue(Tags, TEXT("SOMO.PCG.GenerationId"), Value) ||
			 TryExtractPcgTagValue(Tags, TEXT("SOMO.PCG.Generation"), Value)))
		{
			InOut.GenerationId = Value;
			AddUniqueString(InOut.EvidenceSources, Source + TEXT(":generation_id_tag"));
		}
		if (InOut.SourceComponentPath.IsEmpty() && TryExtractPcgTagValue(Tags, TEXT("SOMO.PCG.SourceComponent"), Value))
		{
			InOut.SourceComponentPath = Value;
			AddUniqueString(InOut.EvidenceSources, Source + TEXT(":source_component_tag"));
		}
		if (InOut.SourceGraphPath.IsEmpty() && TryExtractPcgTagValue(Tags, TEXT("SOMO.PCG.SourceGraph"), Value))
		{
			InOut.SourceGraphPath = Value;
			AddUniqueString(InOut.EvidenceSources, Source + TEXT(":source_graph_tag"));
		}
		if (InOut.GraphHash.IsEmpty() && TryExtractPcgTagValue(Tags, TEXT("SOMO.PCG.GraphHash"), Value))
		{
			InOut.GraphHash = Value;
			AddUniqueString(InOut.EvidenceSources, Source + TEXT(":graph_hash_tag"));
		}
		if (!InOut.bHasSeed && TryExtractPcgTagValue(Tags, TEXT("SOMO.PCG.Seed"), Value))
		{
			int32 ParsedSeed = 0;
			if (TryParseIntStrict(Value, ParsedSeed))
			{
				InOut.Seed = ParsedSeed;
				InOut.bHasSeed = true;
				AddUniqueString(InOut.EvidenceSources, Source + TEXT(":seed_tag"));
			}
		}
	}

	static bool TryInferTileIdFromActorName(AActor* Actor, FString& OutTileId)
	{
		if (!Actor)
		{
			return false;
		}
		TArray<FString> Parts;
		Actor->GetName().ParseIntoArray(Parts, TEXT("_"), true);
		if (Parts.Num() < 2)
		{
			Actor->GetActorLabel().ParseIntoArray(Parts, TEXT("_"), true);
		}
		for (int32 I = Parts.Num() - 2; I >= 0; --I)
		{
			int32 Col = 0;
			int32 Row = 0;
			if (TryParseIntStrict(Parts[I], Col) && TryParseIntStrict(Parts[I + 1], Row))
			{
				OutTileId = FString::Printf(TEXT("coord:%d,%d"), Col, Row);
				return true;
			}
		}
		return false;
	}

	static FPcgGeneratedProvenanceFields BuildPcgGeneratedActorProvenanceFields(
		AActor* Actor,
		AActor* ScopeRoot,
		const FString& RequestedGenerationId = FString(),
		const FString& RequestedTileId = FString())
	{
		FPcgGeneratedProvenanceFields Fields;
		if (Actor)
		{
			MergePcgProvenanceTags(Actor->Tags, TEXT("actor"), Fields);
			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Comp : Components)
			{
				if (Comp)
				{
					MergePcgProvenanceTags(Comp->ComponentTags, TEXT("component"), Fields);
				}
			}
		}

		if (!RequestedTileId.IsEmpty())
		{
			Fields.TileId = RequestedTileId;
			AddUniqueString(Fields.EvidenceSources, TEXT("request:tile_id"));
		}
		if (!RequestedGenerationId.IsEmpty())
		{
			Fields.GenerationId = RequestedGenerationId;
			AddUniqueString(Fields.EvidenceSources, TEXT("request:generation_id"));
		}

		if (Fields.SourceComponentPath.IsEmpty())
		{
			Fields.SourceComponentPath = FirstPcgComponentPath(Actor);
			if (Fields.SourceComponentPath.IsEmpty() && ScopeRoot && ScopeRoot != Actor)
			{
				Fields.SourceComponentPath = FirstPcgComponentPath(ScopeRoot);
			}
			if (!Fields.SourceComponentPath.IsEmpty())
			{
				Fields.bSourceInferred = true;
				AddUniqueString(Fields.EvidenceSources, TEXT("inferred:source_component"));
			}
		}
		if (Fields.SourceGraphPath.IsEmpty())
		{
			Fields.SourceGraphPath = FirstPcgGraphPath(Actor);
			if (Fields.SourceGraphPath.IsEmpty() && ScopeRoot && ScopeRoot != Actor)
			{
				Fields.SourceGraphPath = FirstPcgGraphPath(ScopeRoot);
			}
			if (!Fields.SourceGraphPath.IsEmpty())
			{
				Fields.bSourceInferred = true;
				AddUniqueString(Fields.EvidenceSources, TEXT("inferred:source_graph"));
			}
		}
		if (Fields.TileId.IsEmpty())
		{
			FString InferredTile;
			if (TryInferTileIdFromActorName(Actor, InferredTile))
			{
				Fields.TileId = InferredTile;
				Fields.bTileIdInferred = true;
				AddUniqueString(Fields.EvidenceSources, TEXT("inferred:actor_name_coord"));
			}
		}
		if (Fields.GraphHash.IsEmpty() && !Fields.SourceGraphPath.IsEmpty())
		{
			Fields.GraphHash = StablePcgHashHex(Fields.SourceGraphPath);
			Fields.bGraphHashInferred = true;
			AddUniqueString(Fields.EvidenceSources, TEXT("inferred:graph_hash"));
		}
		if (Fields.GenerationId.IsEmpty())
		{
			const FString Basis = ObjectPathOrEmpty(Actor) + TEXT("|") + Fields.TileId + TEXT("|") + Fields.SourceGraphPath;
			Fields.GenerationId = FString::Printf(TEXT("gen_%s"), *StablePcgHashHex(Basis).Left(12));
			Fields.bGenerationIdInferred = true;
			AddUniqueString(Fields.EvidenceSources, TEXT("inferred:generation_id"));
		}
		if (!Fields.bHasSeed)
		{
			Fields.Seed = StablePcgSeedFromParts(0, Fields.TileId, Fields.GenerationId, Fields.SourceGraphPath, ObjectPathOrEmpty(Actor));
			Fields.bHasSeed = true;
			AddUniqueString(Fields.EvidenceSources, TEXT("inferred:seed"));
		}
		return Fields;
	}

	static TSharedRef<FJsonObject> BuildPcgGeneratedActorProvenanceJson(const FPcgGeneratedProvenanceFields& Fields)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("schema"), GPcgGeneratedProvenanceSchema);
		J->SetStringField(TEXT("tile_id"), Fields.TileId);
		J->SetBoolField(TEXT("tile_id_known"), !Fields.TileId.IsEmpty());
		J->SetBoolField(TEXT("tile_id_inferred"), Fields.bTileIdInferred);
		J->SetStringField(TEXT("generation_id"), Fields.GenerationId);
		J->SetBoolField(TEXT("generation_id_known"), !Fields.GenerationId.IsEmpty());
		J->SetBoolField(TEXT("generation_id_inferred"), Fields.bGenerationIdInferred);
		J->SetNumberField(TEXT("seed"), Fields.Seed);
		J->SetBoolField(TEXT("seed_known"), Fields.bHasSeed);
		J->SetStringField(TEXT("source_component_path"), Fields.SourceComponentPath);
		J->SetStringField(TEXT("source_graph_path"), Fields.SourceGraphPath);
		J->SetStringField(TEXT("graph_hash"), Fields.GraphHash);
		J->SetBoolField(TEXT("source_inferred"), Fields.bSourceInferred);
		J->SetBoolField(TEXT("graph_hash_inferred"), Fields.bGraphHashInferred);
		J->SetArrayField(TEXT("evidence_sources"), StringArrayToJson(Fields.EvidenceSources));
		return J;
	}

	static void AddDependencyRecord(
		TArray<TSharedPtr<FJsonValue>>& Records,
		TSet<FString>& Seen,
		const FString& Category,
		UObject* Asset,
		AActor* Actor,
		UActorComponent* Component,
		const FString& Via)
	{
		if (!Asset)
		{
			return;
		}
		const FString Path = Asset->GetPathName();
		if (Path.IsEmpty())
		{
			return;
		}
		const FString Key = Category + TEXT("|") + Path + TEXT("|") + ObjectPathOrEmpty(Actor) + TEXT("|") + ObjectPathOrEmpty(Component);
		if (Seen.Contains(Key))
		{
			return;
		}
		Seen.Add(Key);
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("category"), Category);
		R->SetStringField(TEXT("asset_path"), Path);
		R->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetName() : FString());
		R->SetStringField(TEXT("source_actor_path"), ObjectPathOrEmpty(Actor));
		R->SetStringField(TEXT("source_component_path"), ObjectPathOrEmpty(Component));
		R->SetStringField(TEXT("via"), Via);
		Records.Add(MakeShared<FJsonValueObject>(R));
	}

	static FString ClassifyDependencyAsset(UObject* Asset)
	{
		if (!Asset || !Asset->GetClass())
		{
			return TEXT("asset");
		}
		if (Asset->IsA<UStaticMesh>() || Asset->IsA<USkeletalMesh>())
		{
			return TEXT("mesh");
		}
		if (Asset->IsA<UMaterialInterface>())
		{
			return TEXT("material");
		}
		const FString ClassName = Asset->GetClass()->GetName();
		if (ClassName.Contains(TEXT("Texture"), ESearchCase::IgnoreCase))
		{
			return TEXT("texture");
		}
		if (ClassName.Contains(TEXT("Anim"), ESearchCase::IgnoreCase) || ClassName.Contains(TEXT("Skeleton"), ESearchCase::IgnoreCase))
		{
			return TEXT("animation");
		}
		if (ClassName.Contains(TEXT("Niagara"), ESearchCase::IgnoreCase))
		{
			return TEXT("niagara");
		}
		if (ClassName.Contains(TEXT("Sound"), ESearchCase::IgnoreCase) || ClassName.Contains(TEXT("Audio"), ESearchCase::IgnoreCase))
		{
			return TEXT("audio");
		}
		if (ClassName.Contains(TEXT("Camera"), ESearchCase::IgnoreCase))
		{
			return TEXT("camera");
		}
		return TEXT("asset");
	}

	static void AddObjectPropertyDependencies(
		TArray<TSharedPtr<FJsonValue>>& Records,
		TSet<FString>& Seen,
		AActor* Actor,
		UActorComponent* Component)
	{
		if (!Component || !Component->GetClass())
		{
			return;
		}
		for (TFieldIterator<FObjectPropertyBase> It(Component->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FObjectPropertyBase* Prop = *It;
			if (!Prop)
			{
				continue;
			}
			UObject* Value = Prop->GetObjectPropertyValue_InContainer(Component);
			if (!Value || Value == Actor || Value == Component)
			{
				continue;
			}
			const FString Path = Value->GetPathName();
			if (!Path.StartsWith(TEXT("/Game/")) && !Path.StartsWith(TEXT("/Engine/")))
			{
				continue;
			}
			AddDependencyRecord(Records, Seen, ClassifyDependencyAsset(Value), Value, Actor, Component, Prop->GetName());
		}
	}

	static void AddHealthIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity,
		const FString& Category,
		AActor* Actor,
		UActorComponent* Component,
		const FString& Detail,
		const FString& RepairHint)
	{
		TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("category"), Category);
		Issue->SetStringField(TEXT("actor_path"), ObjectPathOrEmpty(Actor));
		Issue->SetStringField(TEXT("component_path"), ObjectPathOrEmpty(Component));
		Issue->SetStringField(TEXT("detail"), Detail);
		Issue->SetStringField(TEXT("repair_hint"), RepairHint);
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	static bool HasNearlyZeroScale(const FVector& Scale)
	{
		return FMath::IsNearlyZero(Scale.X) || FMath::IsNearlyZero(Scale.Y) || FMath::IsNearlyZero(Scale.Z);
	}

	static void AuditPrimitiveComponentHealth(AActor* Actor, UPrimitiveComponent* Comp, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Comp)
		{
			return;
		}
		if (HasNearlyZeroScale(Comp->GetComponentScale()))
		{
			AddHealthIssue(Issues, TEXT("error"), TEXT("zero_scale"), Actor, Comp, TEXT("Component has a nearly zero scale axis."), TEXT("Reset component scale before replaying or baking generated output."));
		}
		if (!Comp->IsVisible() && Comp->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
		{
			AddHealthIssue(Issues, TEXT("warning"), TEXT("hidden_collision"), Actor, Comp, TEXT("Component is hidden but collision is still enabled."), TEXT("Either show the component, disable collision, or document it as intentional gameplay collision."));
		}
		const int32 MaterialCount = Comp->GetNumMaterials();
		for (int32 I = 0; I < MaterialCount; ++I)
		{
			if (!Comp->GetMaterial(I))
			{
				AddHealthIssue(Issues, TEXT("warning"), TEXT("invalid_material"), Actor, Comp, FString::Printf(TEXT("Material slot %d is empty."), I), TEXT("Assign a fallback material or regenerate with a validated asset set."));
			}
		}
	}

	static void AddProposedTag(TArray<TSharedPtr<FJsonValue>>& Tags, const FString& Key, const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return;
		}
		Tags.Add(MakeStringJsonValue(FString::Printf(TEXT("%s=%s"), *Key, *Value)));
	}
} // namespace

static bool Tool_PcgMontagePoolValidate(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* PoolJsonPtr = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("montage_pool"), PoolJsonPtr) || !PoolJsonPtr || PoolJsonPtr->Num() == 0)
	{
		OutError = TEXT("Missing required argument: montage_pool (non-empty array)");
		return false;
	}

	TArray<USkeleton*> ExpectedSkeletons;
	const TArray<TSharedPtr<FJsonValue>>* ExpectedSkeletonJson = nullptr;
	if (Arguments->TryGetArrayField(TEXT("expected_skeletons"), ExpectedSkeletonJson) && ExpectedSkeletonJson)
	{
		for (const TSharedPtr<FJsonValue>& V : *ExpectedSkeletonJson)
		{
			const FString Path = V.IsValid() ? V->AsString() : FString();
			if (Path.IsEmpty())
			{
				continue;
			}
			FString LoadErr;
			if (USkeleton* Skeleton = Cast<USkeleton>(Context.Services.LoadAsset(Path, LoadErr)))
			{
				ExpectedSkeletons.AddUnique(Skeleton);
			}
		}
	}

	FString TargetActorId;
	Arguments->TryGetStringField(TEXT("target_actor"), TargetActorId);
	if (!TargetActorId.IsEmpty())
	{
		FString ActorErr;
		if (AActor* TargetActor = Context.Services.FindActorByLabelOrName(TargetActorId, ActorErr))
		{
			TArray<USkeletalMeshComponent*> SkelComps;
			TargetActor->GetComponents<USkeletalMeshComponent>(SkelComps);
			for (USkeletalMeshComponent* Comp : SkelComps)
			{
				if (!Comp)
				{
					continue;
				}
				if (USkeletalMesh* Mesh = Comp->GetSkeletalMeshAsset())
				{
					if (USkeleton* Skeleton = Mesh->GetSkeleton())
					{
						ExpectedSkeletons.AddUnique(Skeleton);
					}
				}
			}
		}
	}

	TArray<FMontagePoolEntry> Pool;
	TArray<double> Cumulative;
	double TotalWeight = 0.0;
	TMap<FString, UAnimMontage*> MontageCache;
	TSharedRef<FJsonObject> PoolValidation = MakeShared<FJsonObject>();
	FString ValidationError;
	const bool bValid = ValidateMontagePoolForDecorate(
		Context, *PoolJsonPtr, Pool, Cumulative, TotalWeight, MontageCache, PoolValidation, ValidationError);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const FMontagePoolEntry& Entry : Pool)
	{
		Entries.Add(MakeShared<FJsonValueObject>(
			BuildMontageAssetInfo(Entry, MontageCache.FindRef(Entry.Path), ExpectedSkeletons, Warnings)));
	}

	TArray<TSharedPtr<FJsonValue>> ExpectedSkeletonPaths;
	for (USkeleton* Skeleton : ExpectedSkeletons)
	{
		ExpectedSkeletonPaths.Add(MakeStringJsonValue(ObjectPathOrEmpty(Skeleton)));
	}

	OutStructured->SetStringField(TEXT("sampler_version"), GMontageSamplerVersion);
	OutStructured->SetBoolField(TEXT("valid"), bValid);
	OutStructured->SetStringField(TEXT("status"), bValid ? TEXT("valid") : TEXT("invalid"));
	OutStructured->SetObjectField(TEXT("pool_validation"), PoolValidation);
	OutStructured->SetArrayField(TEXT("entries"), Entries);
	OutStructured->SetArrayField(TEXT("warnings"), Warnings);
	OutStructured->SetArrayField(TEXT("expected_skeletons_resolved"), ExpectedSkeletonPaths);
	if (!ValidationError.IsEmpty())
	{
		OutStructured->SetStringField(TEXT("validation_error"), ValidationError);
	}

	OutSummary = FString::Printf(TEXT("MontagePoolValidate: %s valid_entries=%d total_weight=%.3f warnings=%d"),
		bValid ? TEXT("valid") : TEXT("invalid"), Pool.Num(), TotalWeight, Warnings.Num());
	return true;
}

static bool Tool_PcgSpawnedActorIndex(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString Scope;
	Arguments->TryGetStringField(TEXT("scope"), Scope);
	if (Scope.IsEmpty())
	{
		Scope = TEXT("children");
	}

	FString ActorId;
	Arguments->TryGetStringField(TEXT("actor"), ActorId);
	AActor* Root = nullptr;
	if (!ActorId.IsEmpty())
	{
		Root = Context.Services.FindActorByLabelOrName(ActorId, OutError);
		if (!Root)
		{
			return false;
		}
	}
	else if (!Scope.Equals(TEXT("all"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("Missing required argument: actor unless scope='all'");
		return false;
	}

	FString WorldError;
	UWorld* World = Root ? Root->GetWorld() : Context.Services.GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError.IsEmpty() ? TEXT("No editor world available") : WorldError;
		return false;
	}

	FString NamePattern;
	Arguments->TryGetStringField(TEXT("actor_name_pattern"), NamePattern);
	const bool bHavePattern = !NamePattern.IsEmpty();

	bool bIncludeEmpty = false;
	Arguments->TryGetBoolField(TEXT("include_empty"), bIncludeEmpty);

	double MaxActorsD = 1000.0;
	Arguments->TryGetNumberField(TEXT("max_actors"), MaxActorsD);
	const int32 MaxActors = FMath::Clamp(static_cast<int32>(MaxActorsD), 1, 100000);
	FString RequestedGenerationId;
	Arguments->TryGetStringField(TEXT("generation_id"), RequestedGenerationId);
	FString RequestedTileId;
	Arguments->TryGetStringField(TEXT("tile_id"), RequestedTileId);

	TArray<AActor*> Candidates;
	CollectIndexActors(World, Root, Scope, Candidates);

	TArray<TSharedPtr<FJsonValue>> Records;
	int32 SkeletalActorCount = 0;
	int32 InstancedActorCount = 0;
	int32 PcgActorCount = 0;
	int32 TotalSkeletalComponents = 0;
	int32 TotalInstancedComponents = 0;
	int32 TotalInstances = 0;
	int32 DurableAssignments = 0;
	int32 CleanupCandidateCount = 0;
	int32 MissingTileIdCount = 0;
	int32 MissingSourceComponentCount = 0;
	int32 MissingSourceGraphCount = 0;

	for (AActor* Actor : Candidates)
	{
		if (!Actor)
		{
			continue;
		}
		if (Records.Num() >= MaxActors)
		{
			break;
		}
		if (bHavePattern && !Actor->GetName().Contains(NamePattern, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TArray<UPCGComponent*> PcgComps;
		TArray<USkeletalMeshComponent*> SkelComps;
		TArray<UInstancedStaticMeshComponent*> IsmComps;
		Actor->GetComponents<UPCGComponent>(PcgComps);
		Actor->GetComponents<USkeletalMeshComponent>(SkelComps);
		Actor->GetComponents<UInstancedStaticMeshComponent>(IsmComps);

		const bool bRelevant = PcgComps.Num() > 0 || SkelComps.Num() > 0 || IsmComps.Num() > 0;
		if (!bRelevant && !bIncludeEmpty)
		{
			continue;
		}

		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("actor"), Actor->GetName());
		R->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		R->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		R->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetName() : FString());
		R->SetArrayField(TEXT("tags"), NamesToJsonArray(Actor->Tags));

		TArray<TSharedPtr<FJsonValue>> PcgComponentRecords;
		for (UPCGComponent* Comp : PcgComps)
		{
			if (!Comp)
			{
				continue;
			}
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("component_path"), Comp->GetPathName());
			C->SetArrayField(TEXT("tags"), NamesToJsonArray(Comp->ComponentTags));
			if (UPCGGraphInterface* GraphInterface = Comp->GetGraphInstance())
			{
				C->SetStringField(TEXT("graph_path"), GraphInterface->GetPathName());
			}
			C->SetBoolField(TEXT("is_generated"), Comp->bGenerated);
			PcgComponentRecords.Add(MakeShared<FJsonValueObject>(C));
		}

		TArray<TSharedPtr<FJsonValue>> SkeletalRecords;
		for (USkeletalMeshComponent* Comp : SkelComps)
		{
			if (!Comp)
			{
				continue;
			}
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("component_path"), Comp->GetPathName());
			C->SetArrayField(TEXT("tags"), NamesToJsonArray(Comp->ComponentTags));
			if (USkeletalMesh* Mesh = Comp->GetSkeletalMeshAsset())
			{
				C->SetStringField(TEXT("mesh"), Mesh->GetPathName());
				C->SetStringField(TEXT("skeleton"), ObjectPathOrEmpty(Mesh->GetSkeleton()));
			}
			C->SetBoolField(TEXT("anim_instance_available"), Comp->GetAnimInstance() != nullptr);
			SkeletalRecords.Add(MakeShared<FJsonValueObject>(C));
		}

		TArray<TSharedPtr<FJsonValue>> InstancedRecords;
		int32 ActorInstances = 0;
		for (UInstancedStaticMeshComponent* Comp : IsmComps)
		{
			if (!Comp)
			{
				continue;
			}
			const int32 InstanceCount = Comp->GetInstanceCount();
			ActorInstances += InstanceCount;
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("component_path"), Comp->GetPathName());
			C->SetStringField(TEXT("component_type"), Comp->IsA<UHierarchicalInstancedStaticMeshComponent>() ? TEXT("HISM") : TEXT("ISM"));
			C->SetArrayField(TEXT("tags"), NamesToJsonArray(Comp->ComponentTags));
			C->SetNumberField(TEXT("instance_count"), InstanceCount);
			if (UStaticMesh* Mesh = Comp->GetStaticMesh())
			{
				C->SetStringField(TEXT("mesh"), Mesh->GetPathName());
			}
			InstancedRecords.Add(MakeShared<FJsonValueObject>(C));
		}

		TSharedRef<FJsonObject> Durable = BuildDurableMontageTagReport(Actor, SkelComps);
		bool bAssigned = false;
		Durable->TryGetBoolField(TEXT("assigned"), bAssigned);
		if (bAssigned)
		{
			++DurableAssignments;
		}
		const FPcgGeneratedProvenanceFields Provenance =
			BuildPcgGeneratedActorProvenanceFields(Actor, Root, RequestedGenerationId, RequestedTileId);
		TSharedRef<FJsonObject> ProvenanceJson = BuildPcgGeneratedActorProvenanceJson(Provenance);
		if (Provenance.TileId.IsEmpty())
		{
			++MissingTileIdCount;
		}
		if (Provenance.SourceComponentPath.IsEmpty())
		{
			++MissingSourceComponentCount;
		}
		if (Provenance.SourceGraphPath.IsEmpty())
		{
			++MissingSourceGraphCount;
		}

		R->SetArrayField(TEXT("pcg_components"), PcgComponentRecords);
		R->SetArrayField(TEXT("skeletal_mesh_components"), SkeletalRecords);
		R->SetArrayField(TEXT("instanced_static_mesh_components"), InstancedRecords);
		R->SetNumberField(TEXT("skeletal_component_count"), SkelComps.Num());
		R->SetNumberField(TEXT("instanced_component_count"), IsmComps.Num());
		R->SetNumberField(TEXT("instance_count"), ActorInstances);
		R->SetObjectField(TEXT("durable_montage_tags"), Durable);
		R->SetStringField(TEXT("tile_id"), Provenance.TileId);
		R->SetStringField(TEXT("generation_id"), Provenance.GenerationId);
		R->SetNumberField(TEXT("seed"), Provenance.Seed);
		R->SetStringField(TEXT("graph_hash"), Provenance.GraphHash);
		R->SetStringField(TEXT("source_component_path"), Provenance.SourceComponentPath);
		R->SetStringField(TEXT("source_graph_path"), Provenance.SourceGraphPath);
		R->SetObjectField(TEXT("generated_actor_provenance"), ProvenanceJson);
		TSharedRef<FJsonObject> ProvenanceHints = BuildComponentProvenanceHints(Actor, PcgComps.Num() > 0);
		ProvenanceHints->SetStringField(TEXT("tile_id"), Provenance.TileId);
		ProvenanceHints->SetStringField(TEXT("generation_id"), Provenance.GenerationId);
		ProvenanceHints->SetStringField(TEXT("graph_hash"), Provenance.GraphHash);
		ProvenanceHints->SetStringField(TEXT("source_component_path"), Provenance.SourceComponentPath);
		ProvenanceHints->SetStringField(TEXT("source_graph_path"), Provenance.SourceGraphPath);
		ProvenanceHints->SetObjectField(TEXT("generated_actor_provenance"), ProvenanceJson);
		R->SetObjectField(TEXT("provenance_hints"), ProvenanceHints);
		TSharedRef<FJsonObject> CleanupEvidence = BuildGeneratedActorCleanupEvidence(Actor, Root, PcgComps.Num() > 0, ActorInstances);
		bool bCleanupCandidate = false;
		CleanupEvidence->TryGetBoolField(TEXT("cleanup_candidate"), bCleanupCandidate);
		if (bCleanupCandidate)
		{
			++CleanupCandidateCount;
		}
		R->SetObjectField(TEXT("cleanup_evidence"), CleanupEvidence);
		Records.Add(MakeShared<FJsonValueObject>(R));

		if (SkelComps.Num() > 0) ++SkeletalActorCount;
		if (IsmComps.Num() > 0) ++InstancedActorCount;
		if (PcgComps.Num() > 0) ++PcgActorCount;
		TotalSkeletalComponents += SkelComps.Num();
		TotalInstancedComponents += IsmComps.Num();
		TotalInstances += ActorInstances;
	}

	OutStructured->SetStringField(TEXT("actor"), ActorId);
	OutStructured->SetStringField(TEXT("scope"), Scope);
	OutStructured->SetStringField(TEXT("root_actor_path"), ObjectPathOrEmpty(Root));
	OutStructured->SetNumberField(TEXT("candidate_count"), Candidates.Num());
	OutStructured->SetNumberField(TEXT("returned_count"), Records.Num());
	OutStructured->SetNumberField(TEXT("skeletal_actor_count"), SkeletalActorCount);
	OutStructured->SetNumberField(TEXT("instanced_actor_count"), InstancedActorCount);
	OutStructured->SetNumberField(TEXT("pcg_actor_count"), PcgActorCount);
	OutStructured->SetNumberField(TEXT("skeletal_component_count"), TotalSkeletalComponents);
	OutStructured->SetNumberField(TEXT("instanced_component_count"), TotalInstancedComponents);
	OutStructured->SetNumberField(TEXT("total_instance_count"), TotalInstances);
	OutStructured->SetNumberField(TEXT("durable_montage_assignment_count"), DurableAssignments);
	OutStructured->SetNumberField(TEXT("cleanup_candidate_count"), CleanupCandidateCount);
	OutStructured->SetNumberField(TEXT("missing_tile_id_count"), MissingTileIdCount);
	OutStructured->SetNumberField(TEXT("missing_source_component_count"), MissingSourceComponentCount);
	OutStructured->SetNumberField(TEXT("missing_source_graph_count"), MissingSourceGraphCount);
	OutStructured->SetObjectField(TEXT("tile_cap_policy"), BuildPcgTileCapPolicy(TEXT("spawned_actor_index_read_only")));
	OutStructured->SetBoolField(TEXT("cleanup_performed"), false);
	OutStructured->SetStringField(TEXT("cleanup_schema"), TEXT("somol.pcg.generated_actor_cleanup.v1"));
	OutStructured->SetStringField(TEXT("provenance_schema"), GPcgGeneratedProvenanceSchema);
	PcgExecutionSafety::AttachPcgEditorSubwindowCleanupEvidence(OutStructured, TEXT("pcg_spawned_actor_index"));
	OutStructured->SetArrayField(TEXT("actors"), Records);

	OutSummary = FString::Printf(
		TEXT("SpawnedActorIndex: scope=%s candidates=%d returned=%d skeletal_actors=%d instanced_actors=%d instances=%d durable_montage=%d"),
		*Scope, Candidates.Num(), Records.Num(), SkeletalActorCount, InstancedActorCount, TotalInstances, DurableAssignments);
	return true;
}

static bool Tool_PcgTileSeedResolve(
	const FSololmcpToolExecutionContext& /*Context*/,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	TArray<FPcgTileDescriptor> Tiles;
	FString TileSource;
	bool bTruncated = false;
	if (!ReadPcgTileDescriptorsFromArguments(Arguments, Tiles, TileSource, bTruncated, OutError))
	{
		return false;
	}
	if (Tiles.Num() == 0)
	{
		FString TileId;
		if (Arguments->TryGetStringField(TEXT("tile_id"), TileId) && !TileId.TrimStartAndEnd().IsEmpty())
		{
			FPcgTileDescriptor Tile;
			Tile.TileId = TileId.TrimStartAndEnd();
			AddUniqueString(Tile.ActorHints, Tile.TileId);
			Tiles.Add(Tile);
			TileSource = TEXT("tile_id");
		}
	}
	if (Tiles.Num() == 0)
	{
		OutError = TEXT("Provide tile_id, tiles_requested[], tile_indices[], allowed_tiles[], or AOI bounds.");
		return false;
	}

	FString GraphPath;
	Arguments->TryGetStringField(TEXT("graph_path"), GraphPath);
	if (GraphPath.IsEmpty())
	{
		Arguments->TryGetStringField(TEXT("asset_path"), GraphPath);
	}
	FString ActorId;
	Arguments->TryGetStringField(TEXT("actor"), ActorId);
	const FString GenerationId = ResolvePcgGenerationId(Arguments, ActorId, GraphPath);
	double BaseSeedD = 0.0;
	Arguments->TryGetNumberField(TEXT("base_seed"), BaseSeedD);
	const int32 BaseSeed = static_cast<int32>(BaseSeedD);
	FString Salt;
	Arguments->TryGetStringField(TEXT("salt"), Salt);

	TArray<TSharedPtr<FJsonValue>> SeedRecords;
	for (const FPcgTileDescriptor& Tile : Tiles)
	{
		TSharedRef<FJsonObject> R = BuildPcgTileDescriptorJson(Tile, GenerationId, GraphPath, BaseSeed, Salt);
		TSharedRef<FJsonObject> Provenance = MakeShared<FJsonObject>();
		Provenance->SetStringField(TEXT("schema"), GPcgGeneratedProvenanceSchema);
		Provenance->SetStringField(TEXT("tile_id"), Tile.TileId);
		Provenance->SetStringField(TEXT("generation_id"), GenerationId);
		Provenance->SetStringField(TEXT("source_graph_path"), GraphPath);
		Provenance->SetStringField(TEXT("graph_hash"), GraphPath.IsEmpty() ? FString() : StablePcgHashHex(GraphPath));
		Provenance->SetNumberField(TEXT("seed"), R->GetNumberField(TEXT("seed")));
		R->SetObjectField(TEXT("provenance_hints"), Provenance);
		SeedRecords.Add(MakeShared<FJsonValueObject>(R));
	}

	OutStructured->SetStringField(TEXT("schema"), TEXT("somol.pcg.tile_seed_resolve.v1"));
	OutStructured->SetStringField(TEXT("tile_source"), TileSource);
	OutStructured->SetBoolField(TEXT("truncated"), bTruncated);
	OutStructured->SetStringField(TEXT("generation_id"), GenerationId);
	OutStructured->SetStringField(TEXT("graph_path"), GraphPath);
	OutStructured->SetNumberField(TEXT("base_seed"), BaseSeed);
	OutStructured->SetNumberField(TEXT("tile_count"), Tiles.Num());
	OutStructured->SetObjectField(TEXT("tile_cap_policy"), BuildPcgTileCapPolicy(TEXT("seed_resolve_read_only")));
	OutStructured->SetArrayField(TEXT("tiles"), SeedRecords);
	if (Tiles.Num() > 0)
	{
		OutStructured->SetStringField(TEXT("tile_id"), Tiles[0].TileId);
	}

	OutSummary = FString::Printf(TEXT("PcgTileSeedResolve: tiles=%d generation_id=%s"), Tiles.Num(), *GenerationId);
	return true;
}

static bool Tool_PcgTileBatchPlan(
	const FSololmcpToolExecutionContext& /*Context*/,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	TArray<FPcgTileDescriptor> Tiles;
	FString TileSource;
	bool bTruncated = false;
	if (!ReadPcgTileDescriptorsFromArguments(Arguments, Tiles, TileSource, bTruncated, OutError))
	{
		return false;
	}
	if (Tiles.Num() == 0)
	{
		OutError = TEXT("Provide tiles_requested[], tile_indices[], allowed_tiles[], or AOI bounds to plan tile batches.");
		return false;
	}

	double BatchSizeD = PcgExecutionSafety::PcgMaxTilesPerGenerate;
	Arguments->TryGetNumberField(TEXT("max_tiles_per_batch"), BatchSizeD);
	const int32 MaxTilesPerBatch = FMath::Clamp(static_cast<int32>(BatchSizeD), 1, PcgExecutionSafety::PcgMaxTilesPerGenerate);

	FString GraphPath;
	Arguments->TryGetStringField(TEXT("graph_path"), GraphPath);
	if (GraphPath.IsEmpty())
	{
		Arguments->TryGetStringField(TEXT("asset_path"), GraphPath);
	}
	FString ActorId;
	Arguments->TryGetStringField(TEXT("actor"), ActorId);
	const FString GenerationId = ResolvePcgGenerationId(Arguments, ActorId, GraphPath);
	double BaseSeedD = 0.0;
	Arguments->TryGetNumberField(TEXT("base_seed"), BaseSeedD);
	const int32 BaseSeed = static_cast<int32>(BaseSeedD);
	FString Salt;
	Arguments->TryGetStringField(TEXT("salt"), Salt);

	TArray<TSharedPtr<FJsonValue>> TileRecords;
	for (const FPcgTileDescriptor& Tile : Tiles)
	{
		TileRecords.Add(MakeShared<FJsonValueObject>(BuildPcgTileDescriptorJson(Tile, GenerationId, GraphPath, BaseSeed, Salt)));
	}

	TArray<TSharedPtr<FJsonValue>> Batches;
	for (int32 BatchStart = 0; BatchStart < Tiles.Num(); BatchStart += MaxTilesPerBatch)
	{
		const int32 BatchIndex = Batches.Num();
		const int32 BatchEnd = FMath::Min(BatchStart + MaxTilesPerBatch, Tiles.Num());
		TArray<TSharedPtr<FJsonValue>> BatchTiles;
		TArray<FString> BatchTileIds;
		for (int32 I = BatchStart; I < BatchEnd; ++I)
		{
			BatchTiles.Add(MakeShared<FJsonValueObject>(BuildPcgTileDescriptorJson(Tiles[I], GenerationId, GraphPath, BaseSeed, Salt)));
			AddUniqueString(BatchTileIds, Tiles[I].TileId);
		}

		TSharedRef<FJsonObject> Batch = MakeShared<FJsonObject>();
		Batch->SetStringField(TEXT("batch_id"), FString::Printf(TEXT("%s_batch_%03d"), *GenerationId, BatchIndex));
		Batch->SetNumberField(TEXT("batch_index"), BatchIndex);
		Batch->SetStringField(TEXT("generation_id"), GenerationId);
		Batch->SetNumberField(TEXT("tile_count"), BatchTiles.Num());
		Batch->SetArrayField(TEXT("tile_ids"), StringArrayToJson(BatchTileIds));
		Batch->SetArrayField(TEXT("tiles"), BatchTiles);
		Batch->SetObjectField(TEXT("tile_cap_policy"), BuildPcgTileCapPolicy(TEXT("tile_batch_plan_batch")));
		Batch->SetStringField(TEXT("recommended_generate_tool"), TEXT("pcg_generate_async"));
		Batch->SetStringField(TEXT("status"), TEXT("planned"));
		Batches.Add(MakeShared<FJsonValueObject>(Batch));
	}

	OutStructured->SetStringField(TEXT("schema"), GPcgTileBatchSchema);
	OutStructured->SetStringField(TEXT("tool_contract"), TEXT("pcg_tile_batch_plan.v1"));
	OutStructured->SetStringField(TEXT("tile_source"), TileSource);
	OutStructured->SetBoolField(TEXT("truncated"), bTruncated);
	OutStructured->SetStringField(TEXT("generation_id"), GenerationId);
	OutStructured->SetStringField(TEXT("graph_path"), GraphPath);
	OutStructured->SetNumberField(TEXT("tile_count"), Tiles.Num());
	OutStructured->SetNumberField(TEXT("max_tiles_per_batch"), MaxTilesPerBatch);
	OutStructured->SetNumberField(TEXT("batch_count"), Batches.Num());
	OutStructured->SetObjectField(TEXT("tile_cap_policy"), BuildPcgTileCapPolicy(TEXT("tile_batch_plan")));
	OutStructured->SetArrayField(TEXT("tiles"), TileRecords);
	OutStructured->SetArrayField(TEXT("batches"), Batches);
	OutStructured->SetStringField(TEXT("next_step"), TEXT("Submit each batch with <=4 tile evidence; replay failed tile_ids only."));
	if (Tiles.Num() > 0)
	{
		OutStructured->SetStringField(TEXT("tile_id"), Tiles[0].TileId);
	}

	OutSummary = FString::Printf(TEXT("PcgTileBatchPlan: tiles=%d batches=%d max_tiles_per_batch=%d"), Tiles.Num(), Batches.Num(), MaxTilesPerBatch);
	return true;
}

static bool Tool_PcgTileGenerationStatus(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FPcgIndexScanRequest Scan;
	if (!ResolvePcgIndexScanRequest(Context, Arguments, Scan, OutError))
	{
		return false;
	}

	FString RequestedGenerationId;
	Arguments->TryGetStringField(TEXT("generation_id"), RequestedGenerationId);
	FString RequestedTileId;
	Arguments->TryGetStringField(TEXT("tile_id"), RequestedTileId);

	TArray<FPcgTileDescriptor> RequestedTiles;
	FString TileSource;
	bool bTruncated = false;
	if (!ReadPcgTileDescriptorsFromArguments(Arguments, RequestedTiles, TileSource, bTruncated, OutError))
	{
		return false;
	}
	if (RequestedTiles.Num() == 0 && !RequestedTileId.IsEmpty())
	{
		FPcgTileDescriptor T;
		T.TileId = RequestedTileId;
		RequestedTiles.Add(T);
	}

	struct FTileStatusAggregate
	{
		FString TileId;
		FString GenerationId;
		int32 ActorCount = 0;
		int32 InstanceCount = 0;
		int32 PcgComponentCount = 0;
		bool bAnyGenerated = false;
		TArray<FString> GraphPaths;
		TArray<TSharedPtr<FJsonValue>> Actors;
	};

	TMap<FString, FTileStatusAggregate> Aggregates;
	for (const FPcgTileDescriptor& Tile : RequestedTiles)
	{
		FTileStatusAggregate& Agg = Aggregates.FindOrAdd(Tile.TileId);
		Agg.TileId = Tile.TileId;
		Agg.GenerationId = RequestedGenerationId;
	}

	int32 ActorsScanned = 0;
	for (AActor* Actor : Scan.Candidates)
	{
		if (!ActorPassesPcgIndexFilters(Actor, Scan) || ActorsScanned >= Scan.MaxActors)
		{
			continue;
		}

		TArray<UPCGComponent*> PcgComps;
		TArray<USkeletalMeshComponent*> SkelComps;
		TArray<UInstancedStaticMeshComponent*> IsmComps;
		Actor->GetComponents<UPCGComponent>(PcgComps);
		Actor->GetComponents<USkeletalMeshComponent>(SkelComps);
		Actor->GetComponents<UInstancedStaticMeshComponent>(IsmComps);
		const bool bRelevant = PcgComps.Num() > 0 || SkelComps.Num() > 0 || IsmComps.Num() > 0;
		if (!bRelevant && !Scan.bIncludeEmpty)
		{
			continue;
		}
		++ActorsScanned;

		const FPcgGeneratedProvenanceFields Provenance =
			BuildPcgGeneratedActorProvenanceFields(Actor, Scan.Root, RequestedGenerationId, RequestedTileId);
		if (!RequestedGenerationId.IsEmpty() && !Provenance.GenerationId.Equals(RequestedGenerationId, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!RequestedTileId.IsEmpty() && !Provenance.TileId.Equals(RequestedTileId, ESearchCase::IgnoreCase))
		{
			continue;
		}

		const FString TileKey = Provenance.TileId.IsEmpty() ? TEXT("unknown") : Provenance.TileId;
		FTileStatusAggregate& Agg = Aggregates.FindOrAdd(TileKey);
		Agg.TileId = TileKey;
		Agg.GenerationId = Provenance.GenerationId;
		++Agg.ActorCount;
		Agg.PcgComponentCount += PcgComps.Num();
		AddUniqueString(Agg.GraphPaths, Provenance.SourceGraphPath);

		int32 ActorInstances = 0;
		for (UInstancedStaticMeshComponent* Comp : IsmComps)
		{
			if (Comp)
			{
				ActorInstances += Comp->GetInstanceCount();
			}
		}
		Agg.InstanceCount += ActorInstances;
		for (UPCGComponent* Comp : PcgComps)
		{
			if (Comp && Comp->bGenerated)
			{
				Agg.bAnyGenerated = true;
			}
		}

		TSharedRef<FJsonObject> ActorRecord = MakeShared<FJsonObject>();
		ActorRecord->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		ActorRecord->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		ActorRecord->SetNumberField(TEXT("instance_count"), ActorInstances);
		ActorRecord->SetObjectField(TEXT("provenance_hints"), BuildPcgGeneratedActorProvenanceJson(Provenance));
		Agg.Actors.Add(MakeShared<FJsonValueObject>(ActorRecord));
	}

	TArray<TSharedPtr<FJsonValue>> TileStatuses;
	for (const auto& Pair : Aggregates)
	{
		const FTileStatusAggregate& Agg = Pair.Value;
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("tile_id"), Agg.TileId);
		R->SetStringField(TEXT("generation_id"), Agg.GenerationId);
		R->SetStringField(TEXT("last_generation_id"), Agg.GenerationId);
		R->SetNumberField(TEXT("actor_count"), Agg.ActorCount);
		R->SetNumberField(TEXT("instance_count"), Agg.InstanceCount);
		R->SetNumberField(TEXT("pcg_component_count"), Agg.PcgComponentCount);
		R->SetArrayField(TEXT("graph_paths"), StringArrayToJson(Agg.GraphPaths));
		R->SetArrayField(TEXT("actors"), Agg.Actors);
		R->SetStringField(TEXT("status"), Agg.ActorCount == 0 ? TEXT("pending_or_not_found") : (Agg.bAnyGenerated ? TEXT("generated") : TEXT("indexed")));
		R->SetObjectField(TEXT("tile_cap_policy"), BuildPcgTileCapPolicy(TEXT("tile_generation_status_read_only")));
		TileStatuses.Add(MakeShared<FJsonValueObject>(R));
	}

	OutStructured->SetStringField(TEXT("schema"), TEXT("somol.pcg.tile_generation_status.v1"));
	OutStructured->SetStringField(TEXT("actor"), Scan.ActorId);
	OutStructured->SetStringField(TEXT("scope"), Scan.Scope);
	OutStructured->SetStringField(TEXT("generation_id"), RequestedGenerationId);
	OutStructured->SetStringField(TEXT("tile_id"), RequestedTileId);
	OutStructured->SetNumberField(TEXT("candidate_count"), Scan.Candidates.Num());
	OutStructured->SetNumberField(TEXT("actors_scanned"), ActorsScanned);
	OutStructured->SetNumberField(TEXT("tile_count"), TileStatuses.Num());
	OutStructured->SetObjectField(TEXT("tile_cap_policy"), BuildPcgTileCapPolicy(TEXT("tile_generation_status_read_only")));
	OutStructured->SetArrayField(TEXT("tiles"), TileStatuses);

	OutSummary = FString::Printf(TEXT("PcgTileGenerationStatus: scope=%s tiles=%d actors_scanned=%d"), *Scan.Scope, TileStatuses.Num(), ActorsScanned);
	return true;
}

static bool Tool_PcgGeneratedDependencyGraph(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FPcgIndexScanRequest Scan;
	if (!ResolvePcgIndexScanRequest(Context, Arguments, Scan, OutError))
	{
		return false;
	}

	FString RequestedGenerationId;
	Arguments->TryGetStringField(TEXT("generation_id"), RequestedGenerationId);
	FString RequestedTileId;
	Arguments->TryGetStringField(TEXT("tile_id"), RequestedTileId);

	TArray<TSharedPtr<FJsonValue>> Dependencies;
	TSet<FString> SeenDependencies;
	TArray<TSharedPtr<FJsonValue>> ActorRecords;
	int32 ActorsScanned = 0;
	for (AActor* Actor : Scan.Candidates)
	{
		if (!ActorPassesPcgIndexFilters(Actor, Scan) || ActorsScanned >= Scan.MaxActors)
		{
			continue;
		}
		TArray<UPCGComponent*> PcgComps;
		TArray<UStaticMeshComponent*> StaticComps;
		TArray<USkeletalMeshComponent*> SkelComps;
		Actor->GetComponents<UPCGComponent>(PcgComps);
		Actor->GetComponents<UStaticMeshComponent>(StaticComps);
		Actor->GetComponents<USkeletalMeshComponent>(SkelComps);
		const bool bRelevant = PcgComps.Num() > 0 || StaticComps.Num() > 0 || SkelComps.Num() > 0;
		if (!bRelevant && !Scan.bIncludeEmpty)
		{
			continue;
		}
		++ActorsScanned;

		const FPcgGeneratedProvenanceFields Provenance =
			BuildPcgGeneratedActorProvenanceFields(Actor, Scan.Root, RequestedGenerationId, RequestedTileId);
		if (!RequestedGenerationId.IsEmpty() && !Provenance.GenerationId.Equals(RequestedGenerationId, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!RequestedTileId.IsEmpty() && !Provenance.TileId.Equals(RequestedTileId, ESearchCase::IgnoreCase))
		{
			continue;
		}

		if (!Provenance.SourceGraphPath.IsEmpty())
		{
			const FString Key = TEXT("pcg_graph|") + Provenance.SourceGraphPath + TEXT("|") + Actor->GetPathName();
			if (!SeenDependencies.Contains(Key))
			{
				SeenDependencies.Add(Key);
				TSharedRef<FJsonObject> GraphDep = MakeShared<FJsonObject>();
				GraphDep->SetStringField(TEXT("category"), TEXT("pcg_graph"));
				GraphDep->SetStringField(TEXT("asset_path"), Provenance.SourceGraphPath);
				GraphDep->SetStringField(TEXT("asset_class"), TEXT("UPCGGraph"));
				GraphDep->SetStringField(TEXT("source_actor_path"), Actor->GetPathName());
				GraphDep->SetStringField(TEXT("source_component_path"), Provenance.SourceComponentPath);
				GraphDep->SetStringField(TEXT("via"), TEXT("generated_actor_provenance"));
				Dependencies.Add(MakeShared<FJsonValueObject>(GraphDep));
			}
		}

		for (UStaticMeshComponent* Comp : StaticComps)
		{
			if (!Comp)
			{
				continue;
			}
			AddDependencyRecord(Dependencies, SeenDependencies, TEXT("mesh"), Comp->GetStaticMesh(), Actor, Comp, TEXT("static_mesh_component"));
			for (int32 I = 0; I < Comp->GetNumMaterials(); ++I)
			{
				AddDependencyRecord(Dependencies, SeenDependencies, TEXT("material"), Comp->GetMaterial(I), Actor, Comp, FString::Printf(TEXT("material_slot_%d"), I));
			}
			AddObjectPropertyDependencies(Dependencies, SeenDependencies, Actor, Comp);
		}
		for (USkeletalMeshComponent* Comp : SkelComps)
		{
			if (!Comp)
			{
				continue;
			}
			USkeletalMesh* Mesh = Comp->GetSkeletalMeshAsset();
			AddDependencyRecord(Dependencies, SeenDependencies, TEXT("mesh"), Mesh, Actor, Comp, TEXT("skeletal_mesh_component"));
			AddDependencyRecord(Dependencies, SeenDependencies, TEXT("animation"), Mesh ? Mesh->GetSkeleton() : nullptr, Actor, Comp, TEXT("skeleton"));
			if (UClass* AnimClass = Comp->GetAnimClass())
			{
				AddDependencyRecord(Dependencies, SeenDependencies, TEXT("animation"), AnimClass, Actor, Comp, TEXT("anim_class"));
			}
			for (int32 I = 0; I < Comp->GetNumMaterials(); ++I)
			{
				AddDependencyRecord(Dependencies, SeenDependencies, TEXT("material"), Comp->GetMaterial(I), Actor, Comp, FString::Printf(TEXT("material_slot_%d"), I));
			}
			AddObjectPropertyDependencies(Dependencies, SeenDependencies, Actor, Comp);
		}
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Comp : Components)
		{
			AddObjectPropertyDependencies(Dependencies, SeenDependencies, Actor, Comp);
		}

		TSharedRef<FJsonObject> ActorRecord = MakeShared<FJsonObject>();
		ActorRecord->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		ActorRecord->SetStringField(TEXT("tile_id"), Provenance.TileId);
		ActorRecord->SetStringField(TEXT("generation_id"), Provenance.GenerationId);
		ActorRecord->SetObjectField(TEXT("provenance_hints"), BuildPcgGeneratedActorProvenanceJson(Provenance));
		ActorRecords.Add(MakeShared<FJsonValueObject>(ActorRecord));
	}

	OutStructured->SetStringField(TEXT("schema"), TEXT("somol.pcg.generated_dependency_graph.v1"));
	OutStructured->SetStringField(TEXT("actor"), Scan.ActorId);
	OutStructured->SetStringField(TEXT("scope"), Scan.Scope);
	OutStructured->SetStringField(TEXT("tile_id"), RequestedTileId);
	OutStructured->SetStringField(TEXT("generation_id"), RequestedGenerationId);
	OutStructured->SetNumberField(TEXT("actors_scanned"), ActorsScanned);
	OutStructured->SetNumberField(TEXT("dependency_count"), Dependencies.Num());
	OutStructured->SetObjectField(TEXT("tile_cap_policy"), BuildPcgTileCapPolicy(TEXT("generated_dependency_graph_read_only")));
	OutStructured->SetArrayField(TEXT("actors"), ActorRecords);
	OutStructured->SetArrayField(TEXT("dependencies"), Dependencies);

	OutSummary = FString::Printf(TEXT("PcgGeneratedDependencyGraph: actors=%d dependencies=%d"), ActorsScanned, Dependencies.Num());
	return true;
}

static bool Tool_PcgGeneratedActorHealthAudit(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FPcgIndexScanRequest Scan;
	if (!ResolvePcgIndexScanRequest(Context, Arguments, Scan, OutError))
	{
		return false;
	}

	FString RequestedGenerationId;
	Arguments->TryGetStringField(TEXT("generation_id"), RequestedGenerationId);
	FString RequestedTileId;
	Arguments->TryGetStringField(TEXT("tile_id"), RequestedTileId);

	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> ActorRecords;
	int32 ActorsAudited = 0;
	int32 ErrorCount = 0;
	for (AActor* Actor : Scan.Candidates)
	{
		if (!ActorPassesPcgIndexFilters(Actor, Scan) || ActorsAudited >= Scan.MaxActors)
		{
			continue;
		}
		TArray<UStaticMeshComponent*> StaticComps;
		TArray<USkeletalMeshComponent*> SkelComps;
		TArray<UPCGComponent*> PcgComps;
		Actor->GetComponents<UStaticMeshComponent>(StaticComps);
		Actor->GetComponents<USkeletalMeshComponent>(SkelComps);
		Actor->GetComponents<UPCGComponent>(PcgComps);
		const bool bRelevant = StaticComps.Num() > 0 || SkelComps.Num() > 0 || PcgComps.Num() > 0;
		if (!bRelevant && !Scan.bIncludeEmpty)
		{
			continue;
		}

		const FPcgGeneratedProvenanceFields Provenance =
			BuildPcgGeneratedActorProvenanceFields(Actor, Scan.Root, RequestedGenerationId, RequestedTileId);
		if (!RequestedGenerationId.IsEmpty() && !Provenance.GenerationId.Equals(RequestedGenerationId, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!RequestedTileId.IsEmpty() && !Provenance.TileId.Equals(RequestedTileId, ESearchCase::IgnoreCase))
		{
			continue;
		}

		++ActorsAudited;
		const int32 IssuesBefore = Issues.Num();
		if (HasNearlyZeroScale(Actor->GetActorScale3D()))
		{
			AddHealthIssue(Issues, TEXT("error"), TEXT("zero_scale"), Actor, nullptr, TEXT("Actor has a nearly zero scale axis."), TEXT("Reset actor scale before replaying or baking generated output."));
		}
		for (UStaticMeshComponent* Comp : StaticComps)
		{
			if (!Comp)
			{
				continue;
			}
			if (!Comp->GetStaticMesh())
			{
				AddHealthIssue(Issues, TEXT("error"), TEXT("missing_mesh"), Actor, Comp, TEXT("StaticMeshComponent has no StaticMesh asset."), TEXT("Assign a validated fallback mesh or regenerate the tile with a complete asset set."));
			}
			AuditPrimitiveComponentHealth(Actor, Comp, Issues);
		}
		for (USkeletalMeshComponent* Comp : SkelComps)
		{
			if (!Comp)
			{
				continue;
			}
			if (!Comp->GetSkeletalMeshAsset())
			{
				AddHealthIssue(Issues, TEXT("error"), TEXT("missing_mesh"), Actor, Comp, TEXT("SkeletalMeshComponent has no SkeletalMesh asset."), TEXT("Assign a validated skeletal mesh or quarantine this generated actor before animation assignment."));
			}
			AuditPrimitiveComponentHealth(Actor, Comp, Issues);
		}
		for (int32 I = IssuesBefore; I < Issues.Num(); ++I)
		{
			const TSharedPtr<FJsonObject> Issue = Issues[I].IsValid() ? Issues[I]->AsObject() : nullptr;
			FString Severity;
			if (Issue.IsValid() && Issue->TryGetStringField(TEXT("severity"), Severity) && Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
			{
				++ErrorCount;
			}
		}

		TSharedRef<FJsonObject> ActorRecord = MakeShared<FJsonObject>();
		ActorRecord->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		ActorRecord->SetStringField(TEXT("tile_id"), Provenance.TileId);
		ActorRecord->SetStringField(TEXT("generation_id"), Provenance.GenerationId);
		ActorRecord->SetNumberField(TEXT("issue_count"), Issues.Num() - IssuesBefore);
		ActorRecord->SetObjectField(TEXT("provenance_hints"), BuildPcgGeneratedActorProvenanceJson(Provenance));
		ActorRecords.Add(MakeShared<FJsonValueObject>(ActorRecord));
	}

	OutStructured->SetStringField(TEXT("schema"), TEXT("somol.pcg.generated_actor_health_audit.v1"));
	OutStructured->SetStringField(TEXT("actor"), Scan.ActorId);
	OutStructured->SetStringField(TEXT("scope"), Scan.Scope);
	OutStructured->SetStringField(TEXT("tile_id"), RequestedTileId);
	OutStructured->SetStringField(TEXT("generation_id"), RequestedGenerationId);
	OutStructured->SetNumberField(TEXT("actors_audited"), ActorsAudited);
	OutStructured->SetNumberField(TEXT("issue_count"), Issues.Num());
	OutStructured->SetNumberField(TEXT("error_count"), ErrorCount);
	OutStructured->SetStringField(TEXT("health_status"), ErrorCount > 0 ? TEXT("fail") : (Issues.Num() > 0 ? TEXT("warn") : TEXT("pass")));
	OutStructured->SetObjectField(TEXT("tile_cap_policy"), BuildPcgTileCapPolicy(TEXT("generated_actor_health_audit_read_only")));
	OutStructured->SetArrayField(TEXT("actors"), ActorRecords);
	OutStructured->SetArrayField(TEXT("issues"), Issues);

	OutSummary = FString::Printf(TEXT("PcgGeneratedActorHealthAudit: actors=%d issues=%d errors=%d"), ActorsAudited, Issues.Num(), ErrorCount);
	return true;
}

static bool Tool_PcgSpawnedActorIndexRepair(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FPcgIndexScanRequest Scan;
	if (!ResolvePcgIndexScanRequest(Context, Arguments, Scan, OutError))
	{
		return false;
	}

	FString RequestedGenerationId;
	Arguments->TryGetStringField(TEXT("generation_id"), RequestedGenerationId);
	FString RequestedTileId;
	Arguments->TryGetStringField(TEXT("tile_id"), RequestedTileId);

	TArray<TSharedPtr<FJsonValue>> RepairCandidates;
	TSet<FString> LiveActorPaths;
	int32 ActorsScanned = 0;
	for (AActor* Actor : Scan.Candidates)
	{
		if (!ActorPassesPcgIndexFilters(Actor, Scan) || ActorsScanned >= Scan.MaxActors)
		{
			continue;
		}
		TArray<UPCGComponent*> PcgComps;
		TArray<UStaticMeshComponent*> StaticComps;
		TArray<USkeletalMeshComponent*> SkelComps;
		Actor->GetComponents<UPCGComponent>(PcgComps);
		Actor->GetComponents<UStaticMeshComponent>(StaticComps);
		Actor->GetComponents<USkeletalMeshComponent>(SkelComps);
		const bool bRelevant = PcgComps.Num() > 0 || StaticComps.Num() > 0 || SkelComps.Num() > 0;
		if (!bRelevant && !Scan.bIncludeEmpty)
		{
			continue;
		}
		++ActorsScanned;
		LiveActorPaths.Add(Actor->GetPathName());

		const FPcgGeneratedProvenanceFields Provenance =
			BuildPcgGeneratedActorProvenanceFields(Actor, Scan.Root, RequestedGenerationId, RequestedTileId);
		TArray<TSharedPtr<FJsonValue>> ProposedTags;
		if (!HasPcgTagKey(Actor->Tags, TEXT("SOMO.PCG.TileId")))
		{
			AddProposedTag(ProposedTags, TEXT("SOMO.PCG.TileId"), Provenance.TileId);
		}
		if (!HasPcgTagKey(Actor->Tags, TEXT("SOMO.PCG.GenerationId")))
		{
			AddProposedTag(ProposedTags, TEXT("SOMO.PCG.GenerationId"), Provenance.GenerationId);
		}
		if (!HasPcgTagKey(Actor->Tags, TEXT("SOMO.PCG.Seed")))
		{
			AddProposedTag(ProposedTags, TEXT("SOMO.PCG.Seed"), FString::FromInt(Provenance.Seed));
		}
		if (!HasPcgTagKey(Actor->Tags, TEXT("SOMO.PCG.GraphHash")))
		{
			AddProposedTag(ProposedTags, TEXT("SOMO.PCG.GraphHash"), Provenance.GraphHash);
		}
		if (!HasPcgTagKey(Actor->Tags, TEXT("SOMO.PCG.SourceComponent")))
		{
			AddProposedTag(ProposedTags, TEXT("SOMO.PCG.SourceComponent"), Provenance.SourceComponentPath);
		}
		if (!HasPcgTagKey(Actor->Tags, TEXT("SOMO.PCG.SourceGraph")))
		{
			AddProposedTag(ProposedTags, TEXT("SOMO.PCG.SourceGraph"), Provenance.SourceGraphPath);
		}

		if (ProposedTags.Num() > 0)
		{
			TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("actor_path"), Actor->GetPathName());
			Candidate->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			Candidate->SetStringField(TEXT("repair_type"), TEXT("metadata_tags_missing"));
			Candidate->SetStringField(TEXT("risk_level"), TEXT("low_metadata_only"));
			Candidate->SetBoolField(TEXT("delete_required"), false);
			Candidate->SetBoolField(TEXT("mutation_performed"), false);
			Candidate->SetArrayField(TEXT("proposed_actor_tags"), ProposedTags);
			Candidate->SetObjectField(TEXT("provenance_hints"), BuildPcgGeneratedActorProvenanceJson(Provenance));
			RepairCandidates.Add(MakeShared<FJsonValueObject>(Candidate));
		}
	}

	TArray<TSharedPtr<FJsonValue>> StaleEntries;
	const TArray<TSharedPtr<FJsonValue>>* IndexEntries = nullptr;
	if (Arguments->TryGetArrayField(TEXT("index_entries"), IndexEntries) && IndexEntries)
	{
		for (int32 I = 0; I < IndexEntries->Num(); ++I)
		{
			const TSharedPtr<FJsonObject> Entry = (*IndexEntries)[I].IsValid() ? (*IndexEntries)[I]->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				continue;
			}
			FString ActorPath;
			if (!(Entry->TryGetStringField(TEXT("actor_path"), ActorPath) || Entry->TryGetStringField(TEXT("path"), ActorPath)))
			{
				continue;
			}
			if (!LiveActorPaths.Contains(ActorPath))
			{
				TSharedRef<FJsonObject> Stale = MakeShared<FJsonObject>();
				Stale->SetNumberField(TEXT("index"), I);
				Stale->SetStringField(TEXT("actor_path"), ActorPath);
				Stale->SetStringField(TEXT("repair_type"), TEXT("external_index_entry_stale"));
				Stale->SetStringField(TEXT("recommendation"), TEXT("drop this entry from the external/index cache; do not delete UE actors from this tool."));
				Stale->SetBoolField(TEXT("delete_performed"), false);
				StaleEntries.Add(MakeShared<FJsonValueObject>(Stale));
			}
		}
	}

	OutStructured->SetStringField(TEXT("schema"), TEXT("somol.pcg.spawned_actor_index_repair.v1"));
	OutStructured->SetStringField(TEXT("actor"), Scan.ActorId);
	OutStructured->SetStringField(TEXT("scope"), Scan.Scope);
	OutStructured->SetStringField(TEXT("tile_id"), RequestedTileId);
	OutStructured->SetStringField(TEXT("generation_id"), RequestedGenerationId);
	OutStructured->SetNumberField(TEXT("actors_scanned"), ActorsScanned);
	OutStructured->SetNumberField(TEXT("repair_candidate_count"), RepairCandidates.Num());
	OutStructured->SetNumberField(TEXT("stale_entry_count"), StaleEntries.Num());
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);
	OutStructured->SetBoolField(TEXT("delete_performed"), false);
	OutStructured->SetStringField(TEXT("mode"), TEXT("conservative_read_only_repair_plan"));
	OutStructured->SetObjectField(TEXT("tile_cap_policy"), BuildPcgTileCapPolicy(TEXT("spawned_actor_index_repair_read_only")));
	OutStructured->SetArrayField(TEXT("repair_candidates"), RepairCandidates);
	OutStructured->SetArrayField(TEXT("stale_index_entries"), StaleEntries);
	OutStructured->SetStringField(TEXT("next_step"), TEXT("Review proposed metadata tags, then apply with a separate explicit low-risk tag mutation flow if desired."));

	OutSummary = FString::Printf(TEXT("PcgSpawnedActorIndexRepair: actors=%d repair_candidates=%d stale_entries=%d"), ActorsScanned, RepairCandidates.Num(), StaleEntries.Num());
	return true;
}

static bool Tool_PcgCharacterMontageDecorate(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString ActorId;
	if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
	{
		OutError = TEXT("Missing required argument: actor (PCG Volume or scope actor)");
		return false;
	}
	AActor* Root = Context.Services.FindActorByLabelOrName(ActorId, OutError);
	if (!Root) return false;

	FString ApplyTo;
	Arguments->TryGetStringField(TEXT("apply_to"), ApplyTo);

	FString NamePattern;
	Arguments->TryGetStringField(TEXT("actor_name_pattern"), NamePattern);
	const bool bHavePattern = !NamePattern.IsEmpty();

	double SeedD = 0.0;
	Arguments->TryGetNumberField(TEXT("seed"), SeedD);
	const int32 GlobalSeed = static_cast<int32>(SeedD);

	bool bAssignOnly = false;
	Arguments->TryGetBoolField(TEXT("assign_only"), bAssignOnly);

	double MaxActorsD = 256.0;
	Arguments->TryGetNumberField(TEXT("max_actors"), MaxActorsD);
	const int32 MaxActors = FMath::Clamp(static_cast<int32>(MaxActorsD), 1, 100000);

	// Parse and validate montage_pool before touching actors. The sampler keeps
	// the same valid-pool ordering as the Rust/PCGSettings implementation.
	const TArray<TSharedPtr<FJsonValue>>* PoolJsonPtr = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("montage_pool"), PoolJsonPtr) || !PoolJsonPtr || PoolJsonPtr->Num() == 0)
	{
		OutError = TEXT("Missing required argument: montage_pool (non-empty array)");
		return false;
	}
	TArray<FMontagePoolEntry> Pool;
	TArray<double> Cumulative;
	double TotalWeight = 0.0;
	TMap<FString, UAnimMontage*> MontageCache;
	TSharedRef<FJsonObject> PoolValidation = MakeShared<FJsonObject>();
	if (!ValidateMontagePoolForDecorate(
		Context, *PoolJsonPtr, Pool, Cumulative, TotalWeight, MontageCache, PoolValidation, OutError))
	{
		OutStructured->SetStringField(TEXT("sampler_version"), GMontageSamplerVersion);
		OutStructured->SetObjectField(TEXT("pool_validation"), PoolValidation);
		SololmcpError::Set(OutStructured, TEXT("VALIDATION_FAILED"), TEXT("montage_pool"), OutError);
		return false;
	}

	TArray<AActor*> Candidates;
	CollectCandidateActors(Root, ApplyTo, Candidates);

	TArray<TSharedPtr<FJsonValue>> Records;
	int32 DecoratedCount = 0, PlayedCount = 0, SkippedCount = 0, DurableBindingCount = 0;

	for (AActor* A : Candidates)
	{
		if (!A) continue;
		if (DecoratedCount + SkippedCount >= MaxActors) break;
		if (bHavePattern && !A->GetName().Contains(NamePattern, ESearchCase::IgnoreCase)) continue;

		USkeletalMeshComponent* SkelComp = A->FindComponentByClass<USkeletalMeshComponent>();
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		const FString ActorPath = A->GetPathName();
		const int32 ActorSeed = StableActorSeedFromPath(ActorPath);
		R->SetStringField(TEXT("actor"), A->GetName());
		R->SetStringField(TEXT("actor_path"), ActorPath);
		R->SetStringField(TEXT("sampler_version"), GMontageSamplerVersion);
		R->SetNumberField(TEXT("actor_seed"), ActorSeed);

		if (!SkelComp)
		{
			R->SetStringField(TEXT("reason"), TEXT("no_skeletal_mesh_component"));
			R->SetBoolField(TEXT("decorated"), false);
			Records.Add(MakeShared<FJsonValueObject>(R));
			++SkippedCount;
			continue;
		}

		const int32 CombinedSeed = MontageHashCombine(GlobalSeed, ActorSeed);
		FMontageSplitMixRng Rng(CombinedSeed);
		const double Target = Rng.NextUnit() * TotalWeight;
		const int32 PickIndex = MontageWeightedPick(Cumulative, Target);
		const FMontagePoolEntry& Pick = Pool[PickIndex];

		UAnimMontage* Montage = MontageCache.FindRef(Pick.Path);
		const float PlayRateMin = FMath::Max(Pick.PlayRateMin, 0.01f);
		const float PlayRateMax = (Pick.PlayRateMax < PlayRateMin) ? PlayRateMin : Pick.PlayRateMax;
		const float PlayRate = (PlayRateMax > PlayRateMin)
			? (PlayRateMin + static_cast<float>(Rng.NextUnit()) * (PlayRateMax - PlayRateMin))
			: PlayRateMin;

		R->SetStringField(TEXT("montage_path"), Pick.Path);
		R->SetNumberField(TEXT("play_rate"), PlayRate);
		R->SetNumberField(TEXT("pool_index"), Pick.PoolIndex);
		R->SetNumberField(TEXT("raw_pool_index"), Pick.RawIndex);
		R->SetNumberField(TEXT("combined_seed"), CombinedSeed);

		if (!Montage)
		{
			R->SetStringField(TEXT("reason"), TEXT("montage_asset_load_failed"));
			R->SetBoolField(TEXT("decorated"), false);
			Records.Add(MakeShared<FJsonValueObject>(R));
			++SkippedCount;
			continue;
		}

		UAnimInstance* AnimInst = SkelComp->GetAnimInstance();
		R->SetBoolField(TEXT("anim_instance_available"), AnimInst != nullptr);
		if (!AnimInst && !bAssignOnly)
		{
			R->SetStringField(TEXT("reason"), TEXT("no_anim_instance"));
			R->SetBoolField(TEXT("decorated"), false);
			Records.Add(MakeShared<FJsonValueObject>(R));
			++SkippedCount;
			continue;
		}

		TSharedRef<FJsonObject> DurableBinding = PersistMontageDurableBinding(
			A, SkelComp, Pick.Path, PlayRate, CombinedSeed, Pick.PoolIndex, Pick.RawIndex);
		R->SetObjectField(TEXT("durable_binding"), DurableBinding);
		if (DurableBinding->GetBoolField(TEXT("ok")))
		{
			++DurableBindingCount;
		}

		R->SetBoolField(TEXT("decorated"), true);
		++DecoratedCount;

		if (!bAssignOnly)
		{
			const float Duration = AnimInst->Montage_Play(Montage, static_cast<float>(PlayRate));
			const bool bPlayed = (Duration > 0.0f);
			R->SetBoolField(TEXT("played"), bPlayed);
			R->SetNumberField(TEXT("reported_duration"), Duration);
			if (bPlayed) ++PlayedCount;
		}
		else
		{
			R->SetBoolField(TEXT("played"), false);
			R->SetStringField(TEXT("note"), TEXT("assign_only=true: montage choice persisted to actor/component tags; no Montage_Play is called."));
		}

		Records.Add(MakeShared<FJsonValueObject>(R));
	}

	OutStructured->SetStringField(TEXT("actor"), ActorId);
	OutStructured->SetStringField(TEXT("sampler_version"), GMontageSamplerVersion);
	OutStructured->SetStringField(TEXT("apply_to"), ApplyTo.IsEmpty() ? TEXT("children") : ApplyTo);
	OutStructured->SetNumberField(TEXT("candidate_count"), Candidates.Num());
	OutStructured->SetNumberField(TEXT("decorated_count"), DecoratedCount);
	OutStructured->SetNumberField(TEXT("played_count"), PlayedCount);
	OutStructured->SetNumberField(TEXT("skipped_count"), SkippedCount);
	OutStructured->SetNumberField(TEXT("durable_binding_count"), DurableBindingCount);
	OutStructured->SetNumberField(TEXT("pool_size"), Pool.Num());
	OutStructured->SetNumberField(TEXT("seed"), GlobalSeed);
	OutStructured->SetBoolField(TEXT("assign_only"), bAssignOnly);
	OutStructured->SetObjectField(TEXT("pool_validation"), PoolValidation);
	OutStructured->SetArrayField(TEXT("records"), Records);

	OutSummary = FString::Printf(
		TEXT("MontageDecorate: actor='%s' candidates=%d decorated=%d played=%d skipped=%d pool=%d seed=%d%s"),
		*ActorId, Candidates.Num(), DecoratedCount, PlayedCount, SkippedCount, Pool.Num(),
		GlobalSeed, bAssignOnly ? TEXT(" (assign_only)") : TEXT(""));
	if (DecoratedCount == 0)
	{
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("actor"),
			TEXT("No skeletal mesh actors/components were decorated."));
		OutError = FString::Printf(TEXT("No montage decorations applied for actor '%s'."), *ActorId);
		return false;
	}
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Registration
// ═══════════════════════════════════════════════════════════════════════════════
void RegisterPcgEnhancementTools(FSololmcpToolRegistry& Registry)
{
	// A8 — pcg_node_catalog
	Registry.Register({
		TEXT("pcg_node_catalog"),
		TEXT("Enumerate all UPCGSettings subclasses with pin signatures. "
			 "Use this BEFORE calling pcg_graph_add_node to avoid invalid class names. "
			 "Supports name_prefix / category filter."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("name_prefix"), FSololmcpSchemaBuilder::String(TEXT("Optional class-name prefix filter"))},
			{TEXT("category"),    FSololmcpSchemaBuilder::String(TEXT("Optional category filter: sampling/filter/spawn/attribute/landscape/spline/transform/math/graph/debug/other"))},
			{TEXT("include_pins"),FSololmcpSchemaBuilder::Boolean(TEXT("Include pin signatures (default true)"))},
			{TEXT("limit"),       FSololmcpSchemaBuilder::Integer(TEXT("Max entries (default 500)"))}
		}),
		Tool_PcgNodeCatalog,
		nullptr,
		/*CacheTtlSeconds=*/300
	});

	Registry.Register({
		TEXT("pcg_node_catalog_lookup"),
		TEXT("Search the PCG node catalog by class/path/category/keyword and by input/output pin label or type. "
		     "Use this before pcg_graph_add_node and before repair planning to avoid class-name typos and pin-shape guesses."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("query"),          FSololmcpSchemaBuilder::String(TEXT("Optional keyword searched against class, path, and category."))},
			{TEXT("class"),          FSololmcpSchemaBuilder::String(TEXT("Optional class/path substring filter. Alias: class_contains."))},
			{TEXT("class_contains"), FSololmcpSchemaBuilder::String(TEXT("Optional class/path substring filter."))},
			{TEXT("path"),           FSololmcpSchemaBuilder::String(TEXT("Optional class path substring filter. Alias: path_contains."))},
			{TEXT("path_contains"),  FSololmcpSchemaBuilder::String(TEXT("Optional class path substring filter."))},
			{TEXT("category"),       FSololmcpSchemaBuilder::String(TEXT("Optional category: sampling/filter/spawn/attribute/landscape/spline/transform/math/graph/debug/other."))},
			{TEXT("pin_type"),       FSololmcpSchemaBuilder::String(TEXT("Optional pin type substring, e.g. Point, Surface, Param."))},
			{TEXT("pin_label"),      FSololmcpSchemaBuilder::String(TEXT("Optional pin label substring."))},
			{TEXT("direction"),      FSololmcpSchemaBuilder::String(TEXT("Optional pin direction: input | output | any."), {TEXT("input"), TEXT("output"), TEXT("any")})},
			{TEXT("include_pins"),   FSololmcpSchemaBuilder::Boolean(TEXT("Include pin signatures in matches (default true)."))},
			{TEXT("limit"),          FSololmcpSchemaBuilder::Integer(TEXT("Max matches returned (default 50, cap 500)."))}
		}),
		Tool_PcgNodeCatalogLookup,
		nullptr,
		/*CacheTtlSeconds=*/300,
		FSololmcpSchemaBuilder::Object({
			{TEXT("matches"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))},
			{TEXT("total_returned"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("candidate_count"), FSololmcpSchemaBuilder::Integer()}
		})
	});

	// A1 — pcg_graph_validate
	Registry.Register({
		TEXT("pcg_graph_validate"),
		TEXT("Pre-execution validation of a UPCGGraph: pin type compatibility, dangling required inputs, "
			 "same-graph edges, graph cycles, missing CDOs, unused outputs, and required-pin audit. Returns stable issue ids. "
			 "REQUIRED: call this before every pcg_generate to save 5-8 minute dead runs."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG Graph asset path"))},
			{TEXT("strictness"), FSololmcpSchemaBuilder::String(TEXT("error (default) | warn | permissive. warn/permissive downgrade blocking issues in the receipt but still expose original_severity."), {TEXT("error"), TEXT("warn"), TEXT("permissive")})}
		}, {TEXT("asset_path")}),
		Tool_PcgGraphValidate,
		nullptr,
		/*CacheTtlSeconds=*/0,  // never cache — graph may have been edited since last call
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("passed"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("error_count"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("warning_count"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("issues"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))},
			{TEXT("required_pin_audit"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("cycles"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))}
		})
	});

	Registry.Register({
		TEXT("pcg_pin_compat_validate"),
		TEXT("Validate whether one PCG output pin can safely connect to one PCG input pin. "
		     "Accepts source_node/source_pin + target_node/target_pin or source_pin_path/target_pin_path as Node::Pin. "
		     "Returns expected/actual types, UE compatibility result, same-graph check, and target capacity."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),       FSololmcpSchemaBuilder::String(TEXT("PCG Graph asset path"))},
			{TEXT("source_node"),      FSololmcpSchemaBuilder::String(TEXT("Source node name/title (output side)."))},
			{TEXT("source_pin"),       FSololmcpSchemaBuilder::String(TEXT("Source output pin label."))},
			{TEXT("source_pin_path"),  FSololmcpSchemaBuilder::String(TEXT("Alias form: SourceNode::SourcePin."))},
			{TEXT("target_node"),      FSololmcpSchemaBuilder::String(TEXT("Target node name/title (input side)."))},
			{TEXT("target_pin"),       FSololmcpSchemaBuilder::String(TEXT("Target input pin label."))},
			{TEXT("target_pin_path"),  FSololmcpSchemaBuilder::String(TEXT("Alias form: TargetNode::TargetPin."))}
		}, {TEXT("asset_path")}),
		Tool_PcgPinCompatValidate,
		nullptr,
		/*CacheTtlSeconds=*/0,
		FSololmcpSchemaBuilder::Object({
			{TEXT("compatible"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("source"), FSololmcpSchemaBuilder::String()},
			{TEXT("target"), FSololmcpSchemaBuilder::String()},
			{TEXT("compatibility_result"), FSololmcpSchemaBuilder::Object({})}
		})
	});

	Registry.Register({
		TEXT("pcg_graph_cycle_detect"),
		TEXT("Detect directed cycles in a UPCGGraph and return explicit node paths for each cycle. "
		     "Use before generate and in auto-repair planning when pcg_graph_validate reports graph_cycle."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG Graph asset path"))}
		}, {TEXT("asset_path")}),
		Tool_PcgGraphCycleDetect,
		nullptr,
		/*CacheTtlSeconds=*/0,
		FSololmcpSchemaBuilder::Object({
			{TEXT("cycle_free"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("cycle_count"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("cycles"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))}
		})
	});

	Registry.Register({
		TEXT("pcg_graph_required_pin_audit"),
		TEXT("List every input pin with required/optional/advanced/default-status evidence. "
		     "Only Required pins missing a connection are blocking; optional pins are reported without becoming generate errors."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG Graph asset path"))}
		}, {TEXT("asset_path")}),
		Tool_PcgGraphRequiredPinAudit,
		nullptr,
		/*CacheTtlSeconds=*/0,
		FSololmcpSchemaBuilder::Object({
			{TEXT("missing_required_count"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("audit"), FSololmcpSchemaBuilder::Object({})}
		})
	});

	Registry.Register({
		TEXT("pcg_failure_classify"),
		TEXT("Classify a PCG failure into validation, budget, asset_missing, streaming, generation, index_stale, or unknown. "
		     "Returns confidence, signals, next recommended tool, and read/repair candidates."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("failure_text"), FSololmcpSchemaBuilder::String(TEXT("Raw error/summary text to classify."))},
			{TEXT("error"),        FSololmcpSchemaBuilder::String(TEXT("Optional error string, appended to failure_text."))},
			{TEXT("tool_name"),    FSololmcpSchemaBuilder::String(TEXT("Optional failing tool name."))},
			{TEXT("issues"),       FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Optional pcg_graph_validate issues array."))}
		}),
		Tool_PcgFailureClassify,
		nullptr,
		/*CacheTtlSeconds=*/0,
		FSololmcpSchemaBuilder::Object({
			{TEXT("failure_category"), FSololmcpSchemaBuilder::String()},
			{TEXT("confidence"), FSololmcpSchemaBuilder::Number()},
			{TEXT("signals"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
			{TEXT("repair_candidates"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))}
		})
	});

	Registry.Register({
		TEXT("pcg_graph_auto_repair_plan"),
		TEXT("Read-only PCG graph repair planner. Runs validate when asset_path is provided, classifies failure evidence, "
		     "and emits ordered patch steps with risk, mutating tools, and rollback path. Does not apply changes."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),    FSololmcpSchemaBuilder::String(TEXT("Optional PCG Graph asset path. If present, validate evidence is included."))},
			{TEXT("failure_text"),  FSololmcpSchemaBuilder::String(TEXT("Optional failure text to classify alongside validation issues."))}
		}),
		Tool_PcgGraphAutoRepairPlan,
		nullptr,
		/*CacheTtlSeconds=*/0,
		FSololmcpSchemaBuilder::Object({
			{TEXT("plan_only"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("can_apply"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("failure_category"), FSololmcpSchemaBuilder::String()},
			{TEXT("repair_steps"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))},
			{TEXT("recommended_tools"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}
		})
	});

	// A3 — pcg_graph_explain
	Registry.Register({
		TEXT("pcg_graph_explain"),
		TEXT("Produce a human-readable description of a UPCGGraph by walking from each terminal node "
			 "back to its sources. Use this to understand an unfamiliar graph before modifying it."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG Graph asset path"))}
		}, {TEXT("asset_path")}),
		Tool_PcgGraphExplain,
		nullptr,
		/*CacheTtlSeconds=*/60
	});

	// A9 — pcg_dry_run
	Registry.Register({
		TEXT("pcg_dry_run"),
		TEXT("Heuristic count-only estimator for a UPCGGraph. Use to budget-screen before actually "
			 "generating. Response marks dry_run_status=heuristic_estimate_only, exact_count_available=false, "
			 "and requires post-generate calibration; accuracy is ~+/-50%."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG Graph asset path"))},
			{TEXT("area_m2"),    FSololmcpSchemaBuilder::Number(TEXT("Target area in m^2 (default 10000 = 100x100m)"))},
			{TEXT("default_density_per_m2"), FSololmcpSchemaBuilder::Number(TEXT("Assumed density per m^2 when node doesn't specify (default 0.2)"))}
		}, {TEXT("asset_path")}),
		Tool_PcgDryRun,
		nullptr,
		/*CacheTtlSeconds=*/30
	});

	// A2 — pcg_graph_template_apply (V1: 5 built-in templates)
	// Lambda captures Registry by reference so the impl can re-enter into add_node/connect/set_property.
	Registry.Register({
		TEXT("pcg_graph_template_apply"),
		TEXT("Apply a built-in PCG scenario template (forest_deciduous / grass_field / rock_scatter / "
			 "riverbank / alpine_conifer) to an existing graph. Turns a 200-line authoring session "
			 "into a single call. Next step should be pcg_graph_validate."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("template_id"),       FSololmcpSchemaBuilder::String(TEXT("One of: forest_deciduous | grass_field | rock_scatter | riverbank | alpine_conifer"))},
			{TEXT("target_graph_path"), FSololmcpSchemaBuilder::String(TEXT("Existing PCG Graph asset path (must be created first via pcg_graph_create)"))}
		}, {TEXT("template_id"), TEXT("target_graph_path")}),
		[&Registry](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
		            TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err) -> bool {
			return Tool_PcgGraphTemplateApply_Impl(Registry, Ctx, Args, Out, Sum, Err);
		},
		nullptr,
		/*CacheTtlSeconds=*/0  // never cache — state mutation
	});

	// A10 — pcg_graph_snapshot
	Registry.Register({
		TEXT("pcg_graph_snapshot"),
		TEXT("Take an atomic snapshot of a PCG Graph asset to /Game/PCG/__Snapshots/. "
			 "Restore via pcg_graph_restore. Use before any bulk mutation (template_apply / refactor) "
			 "to give yourself an undo point."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Source PCG Graph asset path"))},
			{TEXT("tag"),        FSololmcpSchemaBuilder::String(TEXT("Optional snapshot tag (default: current unix timestamp)"))}
		}, {TEXT("asset_path")}),
		Tool_PcgGraphSnapshot,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	// A10 — pcg_graph_restore
	Registry.Register({
		TEXT("pcg_graph_restore"),
		TEXT("Restore a snapshot back to a target path. DESTRUCTIVE: deletes target first if force=true. "
			 "Detach PCG Volumes from the target graph before calling."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("snapshot_asset_path"), FSololmcpSchemaBuilder::String(TEXT("Snapshot path from pcg_graph_snapshot"))},
			{TEXT("target_asset_path"),   FSololmcpSchemaBuilder::String(TEXT("Target PCG Graph path to overwrite"))},
			{TEXT("force"),               FSololmcpSchemaBuilder::Boolean(TEXT("If true, overwrites existing target. Default false = safe mode."))}
		}, {TEXT("snapshot_asset_path"), TEXT("target_asset_path")}),
		Tool_PcgGraphRestore,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	// A11 — pcg_generation_budget_set
	Registry.Register({
		TEXT("pcg_generation_budget_set"),
		TEXT("Set a point budget (max_points) for a PCG Graph. pcg_dry_run will attach a budget_status "
			 "field to its response when this is set. Pass max_points=0 to clear. Advisory — harness "
			 "middleware consults this to gate generate calls."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG Graph asset path"))},
			{TEXT("max_points"), FSololmcpSchemaBuilder::Number(TEXT("Max spawned points budget (0 clears)"))}
		}, {TEXT("asset_path"), TEXT("max_points")}),
		Tool_PcgBudgetSet,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	// A11 — pcg_generation_budget_get
	Registry.Register({
		TEXT("pcg_generation_budget_get"),
		TEXT("Query the point budget for a PCG Graph (or all graphs if asset_path omitted)."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional PCG Graph asset path; if omitted, returns the full budget store"))}
		}),
		Tool_PcgBudgetGet,
		nullptr,
		/*CacheTtlSeconds=*/5
	});

	// A5 — pcg_generate_async
	Registry.Register({
		TEXT("pcg_generate_async"),
		TEXT("Kick off pcg_generate as a background job and return {job_id} immediately. "
		     "Use this whenever you expect generation to take more than a few seconds — "
		     "especially after pcg_dry_run predicts > 100k points. "
		     "Poll via pcg_job_poll. Resolves actor/actor_label + graph_path and forwards canonical graph metadata. "
		     "allowed_tiles/tile_indices are guarded: <=4 requested tiles and exactly one resolved actor scope are required; "
		     "receipts expose tile_filter_enforcement_mode because native pcg_generate still has no tile mask."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"),              FSololmcpSchemaBuilder::String(TEXT("Target actor path/name/label carrying a UPCGComponent"))},
			{TEXT("actor_label"),        FSololmcpSchemaBuilder::String(TEXT("Legacy actor label. Resolved exact-first, with unique partial fallback."))},
			{TEXT("graph_path"),         FSololmcpSchemaBuilder::String(TEXT("Optional PCG Graph asset path. Must match the resolved component graph."))},
			{TEXT("asset_path"),         FSololmcpSchemaBuilder::String(TEXT("Alias for graph_path, matching pcg_graph_validate/dry_run schemas."))},
			{TEXT("allow_all"),          FSololmcpSchemaBuilder::Boolean(TEXT("Required when intentionally generating multiple graph-only or whole-level matches."))},
			{TEXT("allow_partial_actor_label"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow unique partial actor label/name fallback for actor as well as actor_label."))},
			{TEXT("allowed_tiles"),      FSololmcpSchemaBuilder::Array(MakeShared<FJsonObject>(), TEXT("Optional tile ids/descriptors requested by incremental DAGs. Enforces <=4 requested tiles and a single resolved actor scope; receipt mode remains actor-scope guarded, not native tile masked."))},
			{TEXT("tile_indices"),       FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Integer(TEXT("Tile index")), TEXT("Optional execution tile indices requested by incremental DAGs. Capped at <=4; if allowed_tiles is also supplied and comparable, tile_indices must be a subset."))},
			{TEXT("client_request_id"),  FSololmcpSchemaBuilder::String(TEXT("Optional idempotency key — resubmitting the same id returns the existing job"))},
			{TEXT("trace_id"),           FSololmcpSchemaBuilder::String(TEXT("Optional correlation id echoed in job state + events"))}
		}, {}),
		[&Registry](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
		            TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err) -> bool {
			return Tool_PcgGenerateAsync_Impl(Registry, Ctx, Args, Out, Sum, Err);
		},
		nullptr,
		/*CacheTtlSeconds=*/0 // job submission is never cacheable
	});

	// A4 — pcg_partition_preview
	Registry.Register({
		TEXT("pcg_partition_preview"),
		TEXT("Forecast point counts across a tile grid covering an AOI. One-shot replacement for "
		     "manually computing tiles_total + batches_total + per-tile estimated_points in the "
		     "pcg-incremental-fill plan phase. Flags over_budget_tile_count when per_tile_budget is set."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),             FSololmcpSchemaBuilder::String(TEXT("PCG Graph asset path"))},
			{TEXT("min_x"),                  FSololmcpSchemaBuilder::Number(TEXT("AOI min x (meters; same unit as tile_size_m)"))},
			{TEXT("min_y"),                  FSololmcpSchemaBuilder::Number(TEXT("AOI min y"))},
			{TEXT("max_x"),                  FSololmcpSchemaBuilder::Number(TEXT("AOI max x"))},
			{TEXT("max_y"),                  FSololmcpSchemaBuilder::Number(TEXT("AOI max y"))},
			{TEXT("tile_size_m"),            FSololmcpSchemaBuilder::Number(TEXT("Tile edge length in meters (default 256 = PCG Partition default)"))},
			{TEXT("default_density_per_m2"), FSololmcpSchemaBuilder::Number(TEXT("Assumed density per m^2 when graph doesn't specify (default 0.2)"))},
			{TEXT("batch_size"),             FSololmcpSchemaBuilder::Integer(TEXT("Tiles per generate batch (default 4, hard rule 4)"))},
			{TEXT("per_tile_budget"),        FSololmcpSchemaBuilder::Number(TEXT("Optional per-tile max_points; flags over_budget when exceeded"))},
			{TEXT("sample_batches"),         FSololmcpSchemaBuilder::Integer(TEXT("Max batch_samples entries in response (default 16, cap 1024 — all batches share the same per_tile estimate)"))}
		}, {TEXT("asset_path"), TEXT("min_x"), TEXT("min_y"), TEXT("max_x"), TEXT("max_y")}),
		Tool_PcgPartitionPreview,
		nullptr,
		/*CacheTtlSeconds=*/30
	});

	// A6 — pcg_biome_overlay_apply (V2 write + V1 probe fallback)
	Registry.Register({
		TEXT("pcg_biome_overlay_apply"),
		TEXT("v3.7 V2 — Biome overlay writer. Finds UPCGComponent(s) on actor, locates each graph "
		     "instance's FInstancedPropertyBag by reflection, and writes `overrides` via "
		     "SetValueSerializedString. Components whose graph instance has no property bag degrade "
		     "to probe mode for that component. Top-level `mode` is 'write_v2' | 'write_v2_partial' | "
		     "'write_v2_all_failed' | 'write_v2_dry_run' | 'probe_v1' | 'write_v2_noop'. "
		     "B3 composer uses mode to branch between overlay path and sandwich fallback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"),         FSololmcpSchemaBuilder::String(TEXT("Target actor label or name carrying a UPCGComponent"))},
			{TEXT("biome_preset"),  FSololmcpSchemaBuilder::String(TEXT("Optional biome preset name (e.g. 'deciduous_forest', 'alpine_tundra') — echoed into receipt for provenance"))},
			{TEXT("overrides"),     FSololmcpSchemaBuilder::Object({}, {}, TEXT("Parameter name -> value map. Values may be numbers, booleans, or strings; they're written via FInstancedPropertyBag::SetValueSerializedString which parses the string with the same rules as ImportText"))},
			{TEXT("dry_run"),       FSololmcpSchemaBuilder::Boolean(TEXT("If true, only validate that each override property exists on the bag; don't write. Default false"))}
		}, {TEXT("actor")}),
		Tool_PcgBiomeOverlayApply,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	// pcg_resolve_graph_parameters — read-side companion to A6 V2
	Registry.Register({
		TEXT("pcg_resolve_graph_parameters"),
		TEXT("Dump every graph-instance parameter (name / type / current value) for every UPCGComponent "
		     "on an actor. Pair with pcg_biome_overlay_apply(dry_run=true) to plan a write, or with "
		     "pcg_graph_diff to reason about structural vs parameter-level differences."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Target actor carrying one or more UPCGComponent"))}
		}, {TEXT("actor")}),
		Tool_PcgResolveGraphParameters,
		nullptr,
		/*CacheTtlSeconds=*/5
	});

	// pcg_component_info — one-shot state dump
	Registry.Register({
		TEXT("pcg_component_info"),
		TEXT("One-shot state dump of every UPCGComponent on an actor: graph path, is_generated, "
		     "is_partitioned, parameter bag presence, plus the ISM count + total instance count on the "
		     "component's owner as a quick 'how much was spawned here' proxy. Cheaper than sequencing "
		     "pcg_generate_status + actor_list + pcg_graph_get_info by hand."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Target actor"))}
		}, {TEXT("actor")}),
		Tool_PcgComponentInfo,
		nullptr,
		/*CacheTtlSeconds=*/5
	});

	// A12 — pcg_attribute_inspect (V2: include PerInstanceSMCustomData)
	Registry.Register({
		// UE 5.8 guarded read-only probes
		TEXT("pcg_runtime_gen_scheduler_status"),
		TEXT("UE 5.8+ read-only runtime generation scheduler probe. Reports PCG module availability, "
		     "runtime scheduler/subsystem presence, registered execution-source counts, runtime-managed PCG "
		     "component counts, and selected runtime generation CVars. On UE 5.7 it returns status=version_unavailable."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("include_components"), FSololmcpSchemaBuilder::Boolean(TEXT("Include sampled UPCGComponent rows (default false)."))},
			{TEXT("max_components"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum component rows when include_components=true (default 64, cap 512)."))},
			{TEXT("actor_name_pattern"), FSololmcpSchemaBuilder::String(TEXT("Optional actor name/path/label substring filter."))}
		}),
		Tool_PcgRuntimeGenSchedulerStatus,
		nullptr,
		/*CacheTtlSeconds=*/0,
		FSololmcpSchemaBuilder::Object({
			{TEXT("status"), FSololmcpSchemaBuilder::String()},
			{TEXT("engine_version"), FSololmcpSchemaBuilder::String()},
			{TEXT("runtime_scheduler_present"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("registered_execution_source_count"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("runtime_managed_component_count"), FSololmcpSchemaBuilder::Integer()}
		})
	});

	Registry.Register({
		TEXT("pcg_compute_graph_compile_probe"),
		TEXT("UE 5.8+ read-only PCG compute graph compile/cache probe. It does not invoke graph generation, "
		     "graph compilation, or shader compilation; it inspects GPU node intent and checks whether a cached "
		     "compute graph already exists for asset_path/grid_size/compute_graph_index. On UE 5.7 it returns status=version_unavailable."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG Graph asset path. Required on UE 5.8+; ignored by the UE 5.7 version guard."))},
			{TEXT("grid_size"), FSololmcpSchemaBuilder::Integer(TEXT("Optional generation grid size used for cache lookup. Default 0."))},
			{TEXT("compute_graph_index"), FSololmcpSchemaBuilder::Integer(TEXT("Optional cached compute graph index. Default 0."))},
			{TEXT("include_nodes"), FSololmcpSchemaBuilder::Boolean(TEXT("Include per-node GPU intent rows (default true)."))}
		}),
		Tool_PcgComputeGraphCompileProbe,
		nullptr,
		/*CacheTtlSeconds=*/0,
		FSololmcpSchemaBuilder::Object({
			{TEXT("status"), FSololmcpSchemaBuilder::String()},
			{TEXT("engine_version"), FSololmcpSchemaBuilder::String()},
			{TEXT("compile_requested"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("gpu_execution_requested_count"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("cached_compute_graph_present"), FSololmcpSchemaBuilder::Boolean()}
		})
	});

	Registry.Register({
		// A12 - pcg_attribute_inspect (V2: include PerInstanceSMCustomData)
		TEXT("pcg_attribute_inspect"),
		TEXT("Post-generate structural probe: walks ISM/HISM components under an actor (typically a PCG "
		     "Volume) and reports per-mesh instance counts + optional transform samples. V2 adds "
		     "include_custom_data: emits NumCustomDataFloats + PerInstanceSMCustomData sample window + "
		     "per-channel min/max/mean — the real post-generate PCG point attribute view (density / "
		     "scale_variant / biome_weight / material_index channels written by PCG ActorSpawner or "
		     "InstanceSpawner). Use to answer 'what did PCG actually spawn here and with what attributes?' "
		     "without replaying the graph. Pairs with pcg_graph_diff (configured vs realized) and with "
		     "pcg-multi-region-biodiversity (B9) for preset assignment auditing."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"),                    FSololmcpSchemaBuilder::String(TEXT("Target actor (PCG Volume or scope actor whose children carry ISM/HISM)"))},
			{TEXT("sample_limit"),             FSololmcpSchemaBuilder::Integer(TEXT("Max transform samples per ISM component (default 8, cap 256). 0 disables sampling."))},
			{TEXT("include_transforms"),       FSololmcpSchemaBuilder::Boolean(TEXT("Emit transform samples per component (default true)"))},
			{TEXT("mesh_path_contains"),       FSololmcpSchemaBuilder::String(TEXT("Optional case-insensitive substring filter on mesh path — skip ISMs whose mesh path doesn't match"))},
			{TEXT("include_custom_data"),      FSololmcpSchemaBuilder::Boolean(TEXT("V2: if true, emit PerInstanceSMCustomData samples + per-channel stats (default false — backward compat)"))},
			{TEXT("custom_data_sample_limit"), FSololmcpSchemaBuilder::Integer(TEXT("V2: max instances to sample custom-data floats for, per ISM (default 8, cap 256). Ignored when include_custom_data=false."))}
		}, {TEXT("actor")}),
		Tool_PcgAttributeInspect,
		nullptr,
		/*CacheTtlSeconds=*/5
	});

	// pcg_montage_pool_validate
	Registry.Register({
		TEXT("pcg_montage_pool_validate"),
		TEXT("Standalone AnimMontage pool validator for PCG character scatter. Loads every positive-weight "
		     "montage entry, checks weight/play-rate sanity, exposes slot and skeleton basics, and can compare "
		     "against expected_skeletons or a target_actor's skeletal mesh skeletons. Use before "
		     "pcg_character_montage_decorate or before persisting an unattended montage pool."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("montage_pool"),       FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Array of {path, weight, play_rate_min, play_rate_max}"))},
			{TEXT("expected_skeletons"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Skeleton asset path")), TEXT("Optional accepted USkeleton asset paths"))},
			{TEXT("target_actor"),       FSololmcpSchemaBuilder::String(TEXT("Optional actor whose SkeletalMeshComponent skeletons are accepted targets"))}
		}, {TEXT("montage_pool")}),
		Tool_PcgMontagePoolValidate,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	// pcg_spawned_actor_index
	Registry.Register({
		TEXT("pcg_spawned_actor_index"),
		TEXT("Index PCG spawned output around a scope actor. Scans actor/children/attached/all for "
		     "UPCGComponent, SkeletalMeshComponent, ISM, and HISM data, returning actor paths, component "
		     "paths, tags, mesh paths, instance counts, durable SOMO.PCG.Montage.* bindings, and provenance "
		     "hints including tile_id/generation_id/seed/graph_hash. Use before montage assignment and for "
		     "receipt/QA recovery after long PCG batches."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"),              FSololmcpSchemaBuilder::String(TEXT("Scope actor. Required unless scope='all'."))},
			{TEXT("scope"),              FSololmcpSchemaBuilder::String(TEXT("'actor' | 'children' (default) | 'attached' | 'all'. all without actor scans the editor world."))},
			{TEXT("actor_name_pattern"), FSololmcpSchemaBuilder::String(TEXT("Optional case-insensitive substring filter on actor name"))},
			{TEXT("include_empty"),      FSololmcpSchemaBuilder::Boolean(TEXT("If true, include actors with no PCG/Skeletal/ISM/HISM components. Default false."))},
			{TEXT("tile_id"),            FSololmcpSchemaBuilder::String(TEXT("Optional tile id filter/receipt hint."))},
			{TEXT("generation_id"),      FSololmcpSchemaBuilder::String(TEXT("Optional generation id filter/receipt hint."))},
			{TEXT("max_actors"),         FSololmcpSchemaBuilder::Integer(TEXT("Max indexed actors returned (default 1000)."))}
		}),
		Tool_PcgSpawnedActorIndex,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	// Direction 3/6 — tile-batch planning and generated-actor provenance tools.
	Registry.Register({
		TEXT("pcg_tile_batch_plan"),
		TEXT("Plan PCG tile batches from tiles_requested/tile_indices/allowed_tiles or AOI bounds. "
		     "Returns <=4-tile batches, stable per-tile seeds, generation_id, tile_cap_policy, and replay hints; "
		     "does not generate or mutate actors."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("tiles_requested"),    FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Array of tile descriptors: string/id, integer index, [col,row], or object"))},
			{TEXT("tile_indices"),       FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Integer(TEXT("Tile index")), TEXT("Optional tile indices"))},
			{TEXT("allowed_tiles"),      FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Optional tile descriptors already whitelisted by caller"))},
			{TEXT("aoi_min_x_m"),        FSololmcpSchemaBuilder::Number(TEXT("AOI min X in meters"))},
			{TEXT("aoi_min_y_m"),        FSololmcpSchemaBuilder::Number(TEXT("AOI min Y in meters"))},
			{TEXT("aoi_max_x_m"),        FSololmcpSchemaBuilder::Number(TEXT("AOI max X in meters"))},
			{TEXT("aoi_max_y_m"),        FSololmcpSchemaBuilder::Number(TEXT("AOI max Y in meters"))},
			{TEXT("tile_size_m"),        FSololmcpSchemaBuilder::Number(TEXT("Tile size in meters, default 256"))},
			{TEXT("origin_x_m"),         FSololmcpSchemaBuilder::Number(TEXT("Tile grid origin X in meters, default 0"))},
			{TEXT("origin_y_m"),         FSololmcpSchemaBuilder::Number(TEXT("Tile grid origin Y in meters, default 0"))},
			{TEXT("max_tiles_per_batch"),FSololmcpSchemaBuilder::Integer(TEXT("Batch cap, clamped to <=4"))},
			{TEXT("generation_id"),      FSololmcpSchemaBuilder::String(TEXT("Optional generation id echoed into every tile receipt"))},
			{TEXT("graph_path"),         FSololmcpSchemaBuilder::String(TEXT("Optional PCG graph path for seed/provenance hints"))},
			{TEXT("actor"),              FSololmcpSchemaBuilder::String(TEXT("Optional source actor id for generation id fallback"))},
			{TEXT("base_seed"),          FSololmcpSchemaBuilder::Integer(TEXT("Optional base seed"))},
			{TEXT("salt"),               FSololmcpSchemaBuilder::String(TEXT("Optional seed salt"))}
		}),
		Tool_PcgTileBatchPlan,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	Registry.Register({
		TEXT("pcg_tile_generation_status"),
		TEXT("Read-only tile status view over indexed/generated PCG actors. Groups actors by tile_id and generation_id, "
		     "returning actor counts, instance counts, graph hints, and tile_cap_policy."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"),              FSololmcpSchemaBuilder::String(TEXT("Scope actor. Required unless scope='all'."))},
			{TEXT("scope"),              FSololmcpSchemaBuilder::String(TEXT("'actor' | 'children' (default) | 'attached' | 'all'"))},
			{TEXT("tile_id"),            FSololmcpSchemaBuilder::String(TEXT("Optional tile id filter"))},
			{TEXT("generation_id"),      FSololmcpSchemaBuilder::String(TEXT("Optional generation id filter"))},
			{TEXT("tiles_requested"),    FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Optional tile descriptors to pre-seed pending/not-found rows"))},
			{TEXT("actor_name_pattern"), FSololmcpSchemaBuilder::String(TEXT("Optional case-insensitive actor filter"))},
			{TEXT("include_empty"),      FSololmcpSchemaBuilder::Boolean(TEXT("Include actors without PCG/Skeletal/StaticMesh components"))},
			{TEXT("max_actors"),         FSololmcpSchemaBuilder::Integer(TEXT("Max actors scanned (default 1000)"))}
		}),
		Tool_PcgTileGenerationStatus,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	Registry.Register({
		TEXT("pcg_tile_seed_resolve"),
		TEXT("Resolve deterministic per-tile seeds from tile_id/tiles/AOI plus generation_id, graph_path, base_seed, and salt. "
		     "Read-only receipt helper for replay-safe tile generation."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("tile_id"),            FSololmcpSchemaBuilder::String(TEXT("Single tile id"))},
			{TEXT("tiles_requested"),    FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Array of tile descriptors"))},
			{TEXT("tile_indices"),       FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Integer(TEXT("Tile index")), TEXT("Optional tile indices"))},
			{TEXT("generation_id"),      FSololmcpSchemaBuilder::String(TEXT("Optional generation id"))},
			{TEXT("graph_path"),         FSololmcpSchemaBuilder::String(TEXT("Optional PCG graph path"))},
			{TEXT("actor"),              FSololmcpSchemaBuilder::String(TEXT("Optional actor id for generation id fallback"))},
			{TEXT("base_seed"),          FSololmcpSchemaBuilder::Integer(TEXT("Optional base seed"))},
			{TEXT("salt"),               FSololmcpSchemaBuilder::String(TEXT("Optional seed salt"))}
		}),
		Tool_PcgTileSeedResolve,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	Registry.Register({
		TEXT("pcg_generated_dependency_graph"),
		TEXT("Build a read-only dependency graph for generated PCG actors: PCG graph, mesh, material, texture, animation, Niagara, sound/audio, and camera-like asset references where discoverable."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"),              FSololmcpSchemaBuilder::String(TEXT("Scope actor. Required unless scope='all'."))},
			{TEXT("scope"),              FSololmcpSchemaBuilder::String(TEXT("'actor' | 'children' (default) | 'attached' | 'all'"))},
			{TEXT("tile_id"),            FSololmcpSchemaBuilder::String(TEXT("Optional tile id filter"))},
			{TEXT("generation_id"),      FSololmcpSchemaBuilder::String(TEXT("Optional generation id filter"))},
			{TEXT("actor_name_pattern"), FSololmcpSchemaBuilder::String(TEXT("Optional case-insensitive actor filter"))},
			{TEXT("include_empty"),      FSololmcpSchemaBuilder::Boolean(TEXT("Include actors without PCG/Skeletal/StaticMesh components"))},
			{TEXT("max_actors"),         FSololmcpSchemaBuilder::Integer(TEXT("Max actors scanned (default 1000)"))}
		}),
		Tool_PcgGeneratedDependencyGraph,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	Registry.Register({
		TEXT("pcg_generated_actor_health_audit"),
		TEXT("Audit generated PCG actors for missing mesh, empty material slots, hidden collision, and zero scale. "
		     "Read-only; returns repair hints and provenance receipts."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"),              FSololmcpSchemaBuilder::String(TEXT("Scope actor. Required unless scope='all'."))},
			{TEXT("scope"),              FSololmcpSchemaBuilder::String(TEXT("'actor' | 'children' (default) | 'attached' | 'all'"))},
			{TEXT("tile_id"),            FSololmcpSchemaBuilder::String(TEXT("Optional tile id filter"))},
			{TEXT("generation_id"),      FSololmcpSchemaBuilder::String(TEXT("Optional generation id filter"))},
			{TEXT("actor_name_pattern"), FSololmcpSchemaBuilder::String(TEXT("Optional case-insensitive actor filter"))},
			{TEXT("include_empty"),      FSololmcpSchemaBuilder::Boolean(TEXT("Include actors without PCG/Skeletal/StaticMesh components"))},
			{TEXT("max_actors"),         FSololmcpSchemaBuilder::Integer(TEXT("Max actors scanned (default 1000)"))}
		}),
		Tool_PcgGeneratedActorHealthAudit,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	Registry.Register({
		TEXT("pcg_spawned_actor_index_repair"),
		TEXT("Conservative read-only repair planner for generated actor indexes. Proposes missing SOMO.PCG.* provenance tags "
		     "and stale external index entry removals; it does not delete actors or mutate metadata."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"),              FSololmcpSchemaBuilder::String(TEXT("Scope actor. Required unless scope='all'."))},
			{TEXT("scope"),              FSololmcpSchemaBuilder::String(TEXT("'actor' | 'children' (default) | 'attached' | 'all'"))},
			{TEXT("tile_id"),            FSololmcpSchemaBuilder::String(TEXT("Optional tile id hint/filter"))},
			{TEXT("generation_id"),      FSololmcpSchemaBuilder::String(TEXT("Optional generation id hint/filter"))},
			{TEXT("actor_name_pattern"), FSololmcpSchemaBuilder::String(TEXT("Optional case-insensitive actor filter"))},
			{TEXT("include_empty"),      FSololmcpSchemaBuilder::Boolean(TEXT("Include actors without PCG/Skeletal/StaticMesh components"))},
			{TEXT("max_actors"),         FSololmcpSchemaBuilder::Integer(TEXT("Max actors scanned (default 1000)"))},
			{TEXT("index_entries"),      FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Optional prior index entries with actor_path/path to identify stale cache rows"))}
		}),
		Tool_PcgSpawnedActorIndexRepair,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	// pcg_character_montage_decorate
	Registry.Register({
		TEXT("pcg_character_montage_decorate"),
		TEXT("Post-PCG character decorator: for each SkeletalMeshComponent-bearing actor under `actor`, "
		     "pick an AnimMontage from a weighted pool (deterministic SplitMix64 via seed + actor_seed), optionally Montage_Play "
		     "immediately with a randomized play rate. Use after pcg_generate / pcg_generate_async on a "
		     "Volume whose graph spawns NPCs or crowd fill; gives idle-animation variety without a new "
		     "PCG node class. Each successful assignment is persisted to actor/component tags under SOMO.PCG.Montage.*. "
		     "Safe to call multiple times — each call re-picks from the pool."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"),              FSololmcpSchemaBuilder::String(TEXT("Scope actor (PCG Volume, parent, world outer, etc) — candidates are its attached/owned actors"))},
			{TEXT("montage_pool"),       FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Array of {path, weight, play_rate_min, play_rate_max} — weighted pool of AnimMontage assets to pick from"))},
			{TEXT("actor_name_pattern"), FSololmcpSchemaBuilder::String(TEXT("Optional case-insensitive substring filter on candidate actor names"))},
			{TEXT("seed"),               FSololmcpSchemaBuilder::Integer(TEXT("Deterministic global RNG seed (default 0); combined with per-record actor_seed"))},
			{TEXT("assign_only"),        FSololmcpSchemaBuilder::Boolean(TEXT("If true, persist assignment tags and skip Montage_Play (default false = persist + play now)"))},
			{TEXT("max_actors"),         FSololmcpSchemaBuilder::Integer(TEXT("Cap on number of actors decorated in one call (default 256)"))},
			{TEXT("apply_to"),           FSololmcpSchemaBuilder::String(TEXT("Candidate discovery mode: 'children' (attached, default) | 'attached' (owned) | 'all' (both)"))}
		}, {TEXT("actor"), TEXT("montage_pool")}),
		Tool_PcgCharacterMontageDecorate,
		nullptr,
		/*CacheTtlSeconds=*/0
	});

	// A7 — pcg_graph_diff
	Registry.Register({
		TEXT("pcg_graph_diff"),
		TEXT("Structural + property diff between two PCG Graph assets. Compares nodes (by name), "
		     "edges (by from/to node+pin tuple), and per-node settings properties. Canonical consumer: "
		     "pcg_graph_authoring_loop (C3) to check whether an iteration actually changed anything, "
		     "and pcg-biome-composer (B3) to report what a biome preset modified. Pair with "
		     "pcg_graph_snapshot to diff 'before vs after' a refactor."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("base_asset_path"),             FSololmcpSchemaBuilder::String(TEXT("Baseline PCG Graph asset path (the 'before')"))},
			{TEXT("head_asset_path"),             FSololmcpSchemaBuilder::String(TEXT("Comparison PCG Graph asset path (the 'after')"))},
			{TEXT("include_property_diff"),       FSololmcpSchemaBuilder::Boolean(TEXT("Whether to walk node settings properties (default true). Set false to get a cheap structural-only diff."))},
			{TEXT("max_property_diffs_per_node"), FSololmcpSchemaBuilder::Integer(TEXT("Cap on property diffs emitted per node (default 10, cap 500). Keeps response size bounded on nodes with large arrays."))}
		}, {TEXT("base_asset_path"), TEXT("head_asset_path")}),
		Tool_PcgGraphDiff,
		nullptr,
		/*CacheTtlSeconds=*/0  // both sides may have been edited; never cache
	});

	// A5 — pcg_job_poll
	// Lambda captures Registry by reference because AwaitJob drives the tick loop.
	Registry.Register({
		TEXT("pcg_job_poll"),
		TEXT("Poll a PCG job submitted via pcg_generate_async. Returns status / started_at / "
		     "finished_at / error / results. wait_ms>0 piggy-backs on the next completion "
		     "event up to 5000 ms (saves polling chatter). wait_ms=0 is an instant snapshot."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("job_id"),  FSololmcpSchemaBuilder::String(TEXT("Job id returned by pcg_generate_async"))},
			{TEXT("wait_ms"), FSololmcpSchemaBuilder::Integer(TEXT("Max milliseconds to wait server-side (0-5000, default 0)"))}
		}, {TEXT("job_id")}),
		[&Registry](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
		            TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err) -> bool {
			return Tool_PcgJobPoll_Impl(Registry, Ctx, Args, Out, Sum, Err);
		},
		nullptr,
		/*CacheTtlSeconds=*/0
	});
}

} // namespace UE::SOMOLMCP
