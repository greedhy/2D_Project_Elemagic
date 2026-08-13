#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Modules/ModuleManager.h"
#include "Runtime/Launch/Resources/Version.h"

#include "Services/SololmcpEditorServices.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 6
#define SOMOLMCP_NANITE_SETTINGS(Mesh) ((Mesh)->NaniteSettings)
#define SOMOLMCP_SET_NANITE_SETTINGS(Mesh, Settings) ((Mesh)->NaniteSettings = (Settings))
#define SOMOLMCP_LANDSCAPE_LAYER_NAME(Layer) ((Layer)->LayerName)
#define SOMOLMCP_LANDSCAPE_LAYER_NO_WEIGHT(Layer) ((Layer)->bNoWeightBlend)
#define SOMOLMCP_LANDSCAPE_LAYER_HARDNESS(Layer) ((Layer)->Hardness)
#define SOMOLMCP_LANDSCAPE_LAYER_PHYSICAL_MATERIAL(Layer) ((Layer)->PhysMaterial.Get())
#define SOMOLMCP_FIND_OBJECT_EXACT(Type, Outer, Name) FindObject<Type>((Outer), (Name), true)
#else
#define SOMOLMCP_NANITE_SETTINGS(Mesh) ((Mesh)->GetNaniteSettings())
#define SOMOLMCP_SET_NANITE_SETTINGS(Mesh, Settings) ((Mesh)->SetNaniteSettings((Settings)))
#define SOMOLMCP_LANDSCAPE_LAYER_NAME(Layer) ((Layer)->GetLayerName())
#define SOMOLMCP_LANDSCAPE_LAYER_NO_WEIGHT(Layer) ((Layer)->GetBlendMethod() == ELandscapeTargetLayerBlendMethod::None)
#define SOMOLMCP_LANDSCAPE_LAYER_HARDNESS(Layer) ((Layer)->GetHardness())
#define SOMOLMCP_LANDSCAPE_LAYER_PHYSICAL_MATERIAL(Layer) ((Layer)->GetPhysicalMaterial())
#define SOMOLMCP_FIND_OBJECT_EXACT(Type, Outer, Name) FindObject<Type>((Outer), (Name), EFindObjectFlags::ExactClass)
#endif

namespace UE::SOMOLMCP
{
	/**
	 * Python execution is an internal implementation detail, not a public MCP
	 * product surface. Every Python-named entry remains undiscoverable and
	 * uncallable; clients use named native C++ tools through the job queue.
	 */
	bool IsExternalPythonSurfaceToolName(const FString& ToolName);
	bool IsLegacyPythonBackendToolName(const FString& ToolName);

	inline bool ModuleExistsCompat(const TCHAR* ModuleName, FString* OutModulePath = nullptr)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 3
		if (OutModulePath)
		{
			OutModulePath->Reset();
		}
		return FModuleManager::Get().ModuleExists(ModuleName);
#else
		return FModuleManager::Get().ModuleExists(ModuleName, OutModulePath);
#endif
	}

	struct FSololmcpToolExecutionContext
	{
		FSololmcpEditorServices& Services;
	};

	struct FSololmcpToolDefinition
	{
		FString Name;
		FString Description;
		TSharedRef<FJsonObject> InputSchema = MakeShared<FJsonObject>();
		/// Execute handler (required for most tools).
		TFunction<bool(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>&, FString&, FString&)> Execute;
		/// Optional availability check (returns true by default if not set).
		TFunction<bool(const FSololmcpToolExecutionContext&, FString&)> IsAvailable;
		/// Mark as cacheable read-only tool (TTL seconds, 0 = not cacheable).
		int32 CacheTtlSeconds = 0;
		/// Optional output schema for structured response validation (ChiR24-compatible extension).
		TSharedPtr<FJsonObject> OutputSchema;  // null = no schema defined
		/// Legacy implementation invokes Unreal Python. These tools are never exposed or executable.
		bool bUsesExternalPython = false;
	};

	struct FSololmcpToolRuntimeSnapshot
	{
		FString CurrentToolName;
		FString CurrentToolArgumentsPreview;
		FString LastToolName;
		bool bLastToolSuccess = false;
		double LastToolElapsedMs = 0.0;
		int32 ActiveToolExecutions = 0;
		int64 TotalToolExecutionsStarted = 0;
		int64 TotalToolExecutionsCompleted = 0;
		int64 TotalToolExecutionsSucceeded = 0;
		int64 TotalToolExecutionsFailed = 0;
	};

	FSololmcpToolRuntimeSnapshot GetToolRuntimeSnapshot();

	class FSololmcpToolRegistry final
	{
	public:
		FSololmcpToolRegistry();

		void Register(const FSololmcpToolDefinition& Tool);
		bool RegisterNativeCompatibilityAlias(const FString& AliasName, const FString& NativeTargetName);
		// FIXED #11: BuildToolsList 改为非 const，因为通过 IsAvailable 回调可能访问 Services。
		// 去掉原先的 const_cast<FSololmcpEditorServices&>(Services) 破坏 const 正确性的做法。
		TArray<TSharedPtr<FJsonValue>> BuildToolsList();
		bool ExecuteTool(const FString& ToolName, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError);
		bool HasRegisteredTool(const FString& ToolName) const;
		/** 返回所有已注册工具名称的有序列表（按字母升序）。 */
		void GetRegisteredToolNamesSorted(TArray<FString>& OutNames) const;

		/// Clear the response cache (useful after mutations that invalidate cache).
		void ClearResponseCache();

	private:
		FString ComputeCacheKey(const FString& ToolName, const TSharedRef<FJsonObject>& Arguments) const;
		bool TryGetCachedResponse(const FString& CacheKey, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) const;
		void SetCachedResponse(const FString& CacheKey, int32 TtlSeconds, const TSharedRef<FJsonObject>& Structured, const FString& Summary, const FString& Error);

	private:
		FSololmcpEditorServices Services;
		TMap<FString, FSololmcpToolDefinition> ToolsByName;

		// TTL cache: Key → {structured JSON, summary, error, expiry timestamp}
		struct FCachedResponse
		{
			TSharedPtr<FJsonObject> Structured;
			FString Summary;
			FString Error;
			double ExpiryTimeSec = 0.0;
		};
		TMap<FString, FCachedResponse> ResponseCache;
	};

	void RegisterCoreTools(FSololmcpToolRegistry& Registry);
	void RegisterWorldTools(FSololmcpToolRegistry& Registry);
	void RegisterLightingInspectionTools(FSololmcpToolRegistry& Registry);
	void RegisterAssetTools(FSololmcpToolRegistry& Registry);
	void RegisterBlueprintMaterialAnimationTools(FSololmcpToolRegistry& Registry);
	void RegisterSequencerAudioVfxTools(FSololmcpToolRegistry& Registry);
	void RegisterProjectPerceptionTools(FSololmcpToolRegistry& Registry);
	void RegisterScreenshotTools(FSololmcpToolRegistry& Registry);
	void RegisterBlueprintDebugTools(FSololmcpToolRegistry& Registry);
	/// v1.8.0 — 编辑器 UI 自动化工具（菜单点击、模式切换、地形/PCG 创建等）
	void RegisterEditorUITools(FSololmcpToolRegistry& Registry);
	/// Native managed Slate authoring, layout, style, window, extension, and diagnostics surface.
	void RegisterSlateAuthoringTools(FSololmcpToolRegistry& Registry);
	void RegisterUMGCompletionTools(FSololmcpToolRegistry& Registry);
/// v1.9.0 — Asset Toolkit（缩略图传送、资产分析、对比、批量查询、引用查找、安全重命名）
void RegisterAssetToolkitTools(FSololmcpToolRegistry& Registry);
/// v2.1.0 — Terrain/Level Streaming Pipeline + Enhanced Editor Perception（地形高度图导入导出、关卡/子关卡管理、编辑器状态感知）
void RegisterTerrainStreamingTools(FSololmcpToolRegistry& Registry);
/// v3.0 — LOD/HLOD Management（StaticMesh/SkeletalMesh LOD CRUD、WorldPartition HLOD 配置）
void RegisterLodHlodTools(FSololmcpToolRegistry& Registry);
/// v3.0 — Camera Animation & Cinematic（CameraAnim CRUD、Spline 轨迹、渲染队列、视口配置）
void RegisterCinematicTools(FSololmcpToolRegistry& Registry);
/// v3.0 — Post-Processing Effects（PP Volume CRUD、Blendable 材质管理、PP Material 创建）
void RegisterPostProcessTools(FSololmcpToolRegistry& Registry);
/// v2.0.0 — Character Animation Pipeline（identify, retarget, bind, spawn）
void RegisterCharacterAnimationPipelineTools(FSololmcpToolRegistry& Registry);
/// v3.1.0 — Enhanced Tools（missing tools, aliases, PIE, console, Python, material, UMG, VFX, PCG, texture, sequencer）
void RegisterEnhancedTools(FSololmcpToolRegistry& Registry);
/// v3.2.0 — Large World Extension Tools（origin shift, landscape large world, world partition setup, project config for 3000km worlds）
void RegisterLargeWorldTools(FSololmcpToolRegistry& Registry);
void RegisterMaterialTemplateTools(FSololmcpToolRegistry& Registry);
/// v3.14.x - Material semantic graph inspection (read-only): graph explain + property trace.
void RegisterMaterialSemanticTools(FSololmcpToolRegistry& Registry);
void RegisterMaterialGraphPatchTools(FSololmcpToolRegistry& Registry);
void RegisterGameplayContractTools(FSololmcpToolRegistry& Registry);
/// v3.4 — Character Scene Tools (full character setup, ground snap, collision check, batch spawn, animation listing)
void RegisterCharacterSceneTools(FSololmcpToolRegistry& Registry);
/// v3.13.4 - Native explicit-context character assignment, action control, runtime state, root motion, and Motion Warping.
void RegisterCharacterActionRuntimeTools(FSololmcpToolRegistry& Registry);
/// v3.5.0 — Level Prototyping Tools (daynight setup, auto-blockout, surface scatter)
void RegisterLevelPrototypingTools(FSololmcpToolRegistry& Registry);
/// v3.6.0 — Media & Ingest Tools (FileMediaSource/StreamMediaSource/ImgMediaSource/MediaPlayer/MediaTexture/MediaPlaylist + ingest_video/ingest_image_sequence/video_probe)
void RegisterMediaIngestTools(FSololmcpToolRegistry& Registry);
/// v3.7.0 — PCG Enhancement Tools (node_catalog, graph_validate, graph_explain, dry_run) — accuracy/effectiveness/efficiency pack for AI usage of UE5 PCG.
void RegisterPcgEnhancementTools(FSololmcpToolRegistry& Registry);
/// v3.7.1 — Marketplace stubs (fab_search, quixel_search). Returns graceful "requires Epic auth" notices.
void RegisterMarketplaceTools(FSololmcpToolRegistry& Registry);
/// v3.7.1 — Documented-but-missing tools: editor_get_screenshot alias, batch_asset_thumbnails, asset_ingest_from_disk,
/// pcg_snapshot_hash, pcg_snapshot_restore, pcg_troubleshoot, pcg_validate_hook.
void RegisterV371DocumentedGapTools(FSololmcpToolRegistry& Registry);
/// v3.8 — MegaWorld tools (world_mpc_weather_override, landscape_hole_punch, swarm_virtual_detachment_mock)
void RegisterMegaWorldTools(FSololmcpToolRegistry& Registry);
/// v3.9.0 P0-1 — AnimBlueprint StateMachine editing (animbp_create_state_machine / add_state / add_transition / set_transition_rule / add_blendspace_node / list_states)
void RegisterAnimBPStateMachineTools(FSololmcpToolRegistry& Registry);
/// v3.9.0 P0-2 — Curve Asset CRUD (curve_create / add_key / remove_key / set_interpolation / inspect) for UCurveFloat + UCurveLinearColor
void RegisterCurveAssetTools(FSololmcpToolRegistry& Registry);
/// v3.9.0 P0-3+P0-4 — Blueprint control-flow nodes (for_loop / for_each_loop / while_loop / switch_int / switch_enum / select / event)
void RegisterBlueprintFlowTools(FSololmcpToolRegistry& Registry);
/// v3.9.0 P0-5+P0-8 — Blueprint Component panel + batch_edit (add/remove/set_property/list components, blueprint_batch_edit, behaviortree_batch_edit)
void RegisterBlueprintComponentTools(FSololmcpToolRegistry& Registry);
/// v3.10.0 P1-3 — Material Layers (material_layer_create / blend_create / add_layer_stack / set_layer_param / inspect)
void RegisterMaterialLayerTools(FSololmcpToolRegistry& Registry);
/// v3.10.0 P1-4 — Niagara Script Graph node-level (niagara_script_add_node / connect_pins / disconnect_pins / set_node_property / layout_graph)
void RegisterNiagaraScriptGraphTools(FSololmcpToolRegistry& Registry);
/// v3.10.0 P1-5 — StaticMesh editing (set_lod / generate_lods / set_collision / enable_nanite / set_lightmap / inspect)
void RegisterStaticMeshEditTools(FSololmcpToolRegistry& Registry);
/// v3.10.0 P1-6 — Texture processing (set_compression / generate_mips / channel_pack / set_srgb / inspect)
void RegisterTextureProcessTools(FSololmcpToolRegistry& Registry);
/// v3.10.0 P1-7 — MetaSound starter (create / inspect / set_input_default / compile)
void RegisterMetaSoundTools(FSololmcpToolRegistry& Registry);
/// v3.10.0 P2-1 — SkelMesh + PhysicsAsset (skelmesh_set_lod / inspect, physics_asset_create / add_body / add_constraint)
void RegisterSkelMeshPhysicsTools(FSololmcpToolRegistry& Registry);
/// v3.10.0 P2-2 — Landscape advanced (paint_layer / get_layer_weight / spline_create / spline_add_mesh / set_visibility_mask)
void RegisterLandscapeAdvancedTools(FSololmcpToolRegistry& Registry);
/// v3.10.0 P2-3 — WorldPartition (set_cell_size / add_streaming_source / hlod_generate / loading_range_set / inspect)
void RegisterWorldPartitionTools(FSololmcpToolRegistry& Registry);
/// v3.10.0 P2-4+P2-5 — Asset registry references + DataAsset CRUD (asset_find_references / replace_references / fix_redirectors + dataasset_create / set / get_property)
void RegisterAssetRefDataTools(FSololmcpToolRegistry& Registry);
/// v3.10.0 P1-1 — Substrate Material starter (status / inspect / create_simple / set_slab_property) — preview API, returns NOT_AVAILABLE if r.Substrate=0
void RegisterSubstrateMaterialTools(FSololmcpToolRegistry& Registry);
/// v3.11.0 P3-1 — WorldPartition HLOD layer CRUD + force_rebuild
void RegisterWorldPartitionHLODTools(FSololmcpToolRegistry& Registry);
/// v3.11.0 P3-2 — Behavior Tree templates (apply_patrol / apply_combat / list_available)
void RegisterBehaviorTreeTemplateTools(FSololmcpToolRegistry& Registry);
/// v3.11.0 P3-3 — Editor mode switching (editor_mode_enter/exit/get_active/set_tool)
void RegisterEditorModeTools(FSololmcpToolRegistry& Registry);
/// v3.11.0 P3-4 — Plugin + ProjectSettings (plugin_enable/disable + project_settings_get/set)
void RegisterProjectSettingsTools(FSololmcpToolRegistry& Registry);
/// v3.11.0 P3-5 — Sequencer advanced track types (master_track / camera_cuts / fade / event)
void RegisterSequencerAdvancedTools(FSololmcpToolRegistry& Registry);
/// v3.11.0 P3-7 — Named transactions (transaction_begin / end / abort / list)
	void RegisterTransactionTools(FSololmcpToolRegistry& Registry);
	/// Shared native brush contract used by foliage, mesh paint, modeling, fracture, and terrain writers.
	void RegisterBrushKernelTools(FSololmcpToolRegistry& Registry);
	void RegisterFractureAuthoringTools(FSololmcpToolRegistry& Registry);
	void RegisterFractureHierarchyTools(FSololmcpToolRegistry& Registry);
	void RegisterFractureEditTools(FSololmcpToolRegistry& Registry);
	void RegisterFracturePatternTools(FSololmcpToolRegistry& Registry);
	void RegisterChaosDataflowAuthoringTools(FSololmcpToolRegistry& Registry);
	void RegisterMeshPaintAuthoringTools(FSololmcpToolRegistry& Registry);
	void RegisterMeshPaintExtendedTools(FSololmcpToolRegistry& Registry);
	void RegisterMeshPaintTextureTools(FSololmcpToolRegistry& Registry);
/// v3.11.0 P3-8+P4-1+P4-4 — DevOps trio (sourcecontrol_* + cook_/package_ + net_*)
void RegisterDevOpsTools(FSololmcpToolRegistry& Registry);
/// v3.12.0 P3-6 — MCP tool hot-reload (mcp_register_python_tool / unregister / list / reload / get_source)
void RegisterHotReloadTools(FSololmcpToolRegistry& Registry);
/// v3.12.0 P4-2 — ChaosCloth (cloth_inspect / create_from_section / set_simulation_enabled / set_wind / status)
void RegisterClothTools(FSololmcpToolRegistry& Registry);
/// v3.12.0 P4-3 — VR/AR template setup (xr_setup_pawn / add_motion_controllers / add_floor / create_teleport_volume / status)
void RegisterXRTemplateTools(FSololmcpToolRegistry& Registry);
/// v3.12.0 P4-6 — Embodied loop / PIE control (embody_pie_start/stop/state + input_press_key/axis + get_observation)
void RegisterEmbodiedLoopTools(FSololmcpToolRegistry& Registry);
/// v3.13.0 P5 — Cook pipeline (clean / validate_assets / size_report / chunk_assignment / dependency_graph)
void RegisterCookPipelineTools(FSololmcpToolRegistry& Registry);
void RegisterPakUpdateTools(FSololmcpToolRegistry& Registry);
/// v3.13.0 P5 — Plugin discovery (list_all / inspect / check_compatibility / recommend_for_role)
void RegisterPluginDiscoveryTools(FSololmcpToolRegistry& Registry);
/// v3.13.0 P5 — Niagara custom HLSL (add_custom_node / inspect / set_code / validate)
void RegisterNiagaraHLSLTools(FSololmcpToolRegistry& Registry);
/// v3.14.0 — Lite scan tools: world_partition_status_lite, landscape_actor_list_lite.
/// Fast read-only alternatives to python_exec scripts that would otherwise enumerate
/// the full scene / asset registry on the game thread and freeze the editor.
void RegisterLiteScanTools(FSololmcpToolRegistry& Registry);
/// v3.14 — Generic single-statement unreal.* python dispatcher (one tool: unreal_call).
/// Used by the v3.9.x client-side Python sidecar to forward each Python statement
/// to UE without freezing the game thread for the whole script.
void RegisterUnrealCallTool(FSololmcpToolRegistry& Registry);
/// v3.10 Phase D — Batch APIs (5 tools): one game-thread enter for N items.
/// Eliminates per-call MCP overhead for tight client-side loops.
void RegisterBatchTools(FSololmcpToolRegistry& Registry);
/// H2 external UE MCP uplift: modal editor dialog policy/list/respond contracts.
/// First batch is intentionally fail-safe: registration + schemas + receipt-only stubs.
void RegisterEditorDialogTools(FSololmcpToolRegistry& Registry);
/// v3.14.x - Editor build orchestration: lighting, nav/AI data, shaders, reflection captures, packaging profile.
void RegisterEditorBuildPipelineTools(FSololmcpToolRegistry& Registry);
/// v3.15.x - MCP-side execution planning for multi-agent clients: tool profiles, resource locks, and parallel waves.
void RegisterMcpExecutionPlanningTools(FSololmcpToolRegistry& Registry);
/// v3.16.x - Versioned capability probes: UE 5.7 baseline expansion facts plus UE 5.8-only official MCP/toolset gates.
void RegisterVersionedCapabilityTools(FSololmcpToolRegistry& Registry);
/// v3.17.x - UE 5.7+ validation/render/take-recorder production probes and plans.
void RegisterValidationRenderTools(FSololmcpToolRegistry& Registry);
/// v3.17.x - UE 5.7+ virtual-production/ingest probes and plans.
void RegisterVirtualProductionIngestTools(FSololmcpToolRegistry& Registry);
/// v3.17.x - UE 5.7+ World AI, data, CommonUI, and input probes/plans.
void RegisterWorldAiDataUiTools(FSololmcpToolRegistry& Registry);
/// v3.17.x - UE 5.7+ automation, Motion Design, PCG/Niagara, and Editor Utility bridge plans.
void RegisterProductionBridgeTools(FSololmcpToolRegistry& Registry);
/// v3.24.x - UE 5.8 NiagaraToolsets concrete P1 schema/topology/plan/receipt tools without optional-plugin link deps.
void RegisterNiagaraToolsetP1Tools(FSololmcpToolRegistry& Registry);
/// v3.24.x - UE 5.8 MeshTerrainMode/MeshPartition concrete P1 probes, plans, and UE 5.7-safe gates.
void RegisterMeshTerrainModeP1Tools(FSololmcpToolRegistry& Registry);
/// v3.31.x - UE 5.8 native Mesh Terrain/MeshPartition authoring and QA surface.
void RegisterMeshTerrainNativeTools(FSololmcpToolRegistry& Registry);
/// 2026-08-05 - Legacy foliage_*/geometry_script_* names promoted to real native executors.
void RegisterLegacyNativeCompletionTools(FSololmcpToolRegistry& Registry);
/// Mesh Paint upgrade MP-01: native geometry_vertex_color_bake writer promoted ahead of the P0 catalog wrapper.
void RegisterMeshPaintUpgradeTools(FSololmcpToolRegistry& Registry);
void RegisterPcg58NativeTools(FSololmcpToolRegistry& Registry);
void RegisterUE58RenderingTools(FSololmcpToolRegistry& Registry);
void RegisterUE58CharacterAnimationTools(FSololmcpToolRegistry& Registry);
void RegisterUE58SequencerTools(FSololmcpToolRegistry& Registry);
void RegisterUE58ControlRigPhysicsTools(FSololmcpToolRegistry& Registry);
void RegisterUE58ControlRigDynamicsTools(FSololmcpToolRegistry& Registry);
void RegisterUE58DirectMeshControlTools(FSololmcpToolRegistry& Registry);
void RegisterUE58AnimationBridgeTools(FSololmcpToolRegistry& Registry);
void RegisterUE58WorldbuildingBridgeTools(FSololmcpToolRegistry& Registry);
void RegisterUE58ProductionBridgeTools(FSololmcpToolRegistry& Registry);
/// v3.12.7 - Production desktop/video capture preflight and WorldForge presentation audit.
void RegisterVideoAutomationTools(FSololmcpToolRegistry& Registry);
void RegisterVideoProductionUpgradeTools(FSololmcpToolRegistry& Registry);
void RegisterUE58FrameworkBridgeTools(FSololmcpToolRegistry& Registry);
void RegisterUE58DeveloperIterationBridgeTools(FSololmcpToolRegistry& Registry);
void RegisterUE58PlatformBridgeTools(FSololmcpToolRegistry& Registry);
void RegisterUE58EditorMotionBridgeTools(FSololmcpToolRegistry& Registry);
void RegisterUE58VirtualProductionMediaBridgeTools(FSololmcpToolRegistry& Registry);
void RegisterUE58ChaosNiagaraMcpBridgeTools(FSololmcpToolRegistry& Registry);
/// v3.17.x - UE 5.8-only production plans with fail-closed 5.7 route guards.
void RegisterUE58ProductionTools(FSololmcpToolRegistry& Registry);
/// v3.18.x - UE 5.8 ToolsetRegistry / Toolsets inventory, schema, wrapper status, and smoke matrix.
void RegisterUE58ToolsetTools(FSololmcpToolRegistry& Registry);
/// v3.19.x - UE 5.7-safe UE 5.8 callable inventory, 5.7/5.8 diff, module gates, and wrapper candidate ranking.
void RegisterUE58CallableDiffTools(FSololmcpToolRegistry& Registry);
/// v3.18.x - SOMOLMCP-native LandscapePatch and PCG interop coverage, including UE 5.8 MeshPartition gates.
void RegisterLandscapePatchPcgInteropTools(FSololmcpToolRegistry& Registry);
/// v3.19.x - UE 5.7-safe BlueprintCallable reflection bridge: inventory/schema/readonly invoke/allowlist/wrapper plans.
void RegisterBlueprintCallableBridgeTools(FSololmcpToolRegistry& Registry);
/// v3.24.x - UE 5.7+ MetaHuman/Mutable character-customization probes, plans, receipts, and plugin-safe catalogs.
void RegisterCharacterCustomizationP1Tools(FSololmcpToolRegistry& Registry);
/// v3.24.x - UE 5.8 cloth/outfit/dataflow probes, plans, receipts, and UE 5.7-safe gates.
void RegisterClothOutfitDataflowP1Tools(FSololmcpToolRegistry& Registry);
/// v3.20.x - P0 completion wrapper surface for GeometryScript, Sequencer/MRQ, PCG interops, ControlRig, Toolsets, and gameplay basics.
void RegisterP0CompletionTools(FSololmcpToolRegistry& Registry);
/// v3.21.x - P1 production feature wrapper surface: PCG deep interops, MeshTerrain, Niagara Toolset, Mover/UAF, MetaHuman/Mutable/Cloth, Water, World AI, CommonUI.
void RegisterP1CompletionTools(FSololmcpToolRegistry& Registry);
/// v3.22.x - P2 broad editor orchestration wrapper surface: Sequencer, ControlRig, TEDS, material validation, Insights, sandboxing, semantic search, audio, asset scripting.
void RegisterP2CompletionTools(FSololmcpToolRegistry& Registry);
/// v3.23.x - P3 long-tail and experimental wrapper surface: NNE/ML, media, MeshPartition/MeshTerrain deep, DDC, RPC, diagnostics/source-index.
void RegisterP3CompletionTools(FSololmcpToolRegistry& Registry);
/// v3.25.x - SOMOL World Create unattended-import orchestration gates and UE 5.7/5.8 adapters.
void RegisterWorldCreateTools(FSololmcpToolRegistry& Registry);
/// v3.26.x - SOMOL modular architecture, settlement, collision, and enterability gates.
void RegisterArchitectureTools(FSololmcpToolRegistry& Registry);
/// Native source/target project asset migration with dependency closure, immutable staging, resume, rollback, and receipts.
void RegisterCrossProjectAssetTools(FSololmcpToolRegistry& Registry);
/// v3.27.x - WorldForge UE 5.8 MeshPartition exact bridge tool names and fail-closed native writer gates.
void RegisterWorldForgeMeshPartitionBridgeTools(FSololmcpToolRegistry& Registry);
/// v3.28.x - WorldForge UE 5.8 Procedural Vegetation Editor bridge probes/plans/receipt gates.
void RegisterWorldForgePVEBridgeTools(FSololmcpToolRegistry& Registry);
/// v3.29.x - WorldForge RuntimeMap / minimap / exploration fog / map tile orchestration tools.
void RegisterWorldForgeRuntimeMapTools(FSololmcpToolRegistry& Registry);
void RegisterWorldForgeSurfaceMicrostructureTools(FSololmcpToolRegistry& Registry);
void RegisterWorldForgeNavigationPrecisionTools(FSololmcpToolRegistry& Registry);
/// SOMOLAtmosphere M1 batch: director spawn, state query, cloud override, WMO genera presets, noise bake pending gate.
void RegisterWorldForgeAtmosphereTools(FSololmcpToolRegistry& Registry);
void RegisterWorldForgeSupercellTools(FSololmcpToolRegistry& Registry);
void RegisterWorldForgePlanetaryRingTools(FSololmcpToolRegistry& Registry);
void RegisterWorldForgeCelestialSceneTools(FSololmcpToolRegistry& Registry);
void RegisterWorldForgeSkyWeatherTools(FSololmcpToolRegistry& Registry);
/// v3.30.x - Scene/Settlement IR consumer tools for somol-settlement bridge outputs.
void RegisterSettlementTools(FSololmcpToolRegistry& Registry);
/// v3.30.x - Worldspace diagnostics: subtraction toggles (worldspace_debug_toggle) + quantified asserts (worldspace_assert).
void RegisterWorldspaceDiagnosticsTools(FSololmcpToolRegistry& Registry);
/// WorldForge generic platform schema/provider/command discovery and compatibility tools.
void RegisterWorldForgePlatformContractTools(FSololmcpToolRegistry& Registry);
/// SSOT-generated P0 control surface: 14 features x 8 semantics, backed by the shared UE authority subsystem.
void RegisterWorldForgeP0ControlTools(FSololmcpToolRegistry& Registry);
/// SSOT-generated Earth-system live control surface: 9 features x 8 semantics.
void RegisterWorldForgeEarthSystemControlTools(FSololmcpToolRegistry& Registry);
void RegisterWorldForgeEarthSystemBlueprintLiveVerifyTool(FSololmcpToolRegistry& Registry);
void RegisterWorldForgeGranularWaterFXContractTools(FSololmcpToolRegistry& Registry);
void RegisterWorldForgeSwarmTools(FSololmcpToolRegistry& Registry);
/// WorldForge 3.0 domain-enrichment capability contract runtime: 315 frozen contract tools plus list/configure/audit management surface.
void RegisterWorldForgeEnrichmentCapabilityTools(FSololmcpToolRegistry& Registry);
/// Eight MCP semantics projected from the shared layered-coupling runtime.
void RegisterWorldForgeLayeredCouplingTools(FSololmcpToolRegistry& Registry);
/// WorldForge GeoTerrain v1.1: 23 frozen geoterrain tools (WP00 WF-00-05); schemas generated from MCP_TOOL_SCHEMAS_V1_1.json.
	void RegisterWorldForgeGeoTerrainTools(FSololmcpToolRegistry& Registry);
	void RegisterWorldForgeTerrainRepresentationTools(FSololmcpToolRegistry& Registry);
	/// Rev.C P0-MAPPING: canonical MCP/Blueprint ownership and recursively
	/// closed input/output schema audit.
	void RegisterWorldForgeRevCMappingAuditTools(FSololmcpToolRegistry& Registry);
	/// erp14: editor-runtime parity dual-mode audit tool family (10 read-only tools).
	void RegisterWorldForgeEditorRuntimeParityTools(FSololmcpToolRegistry& Registry);
	/// Interchange import/export coverage, batch 1 (Layer A orchestration, 15 tools).
	/// InterchangeCore/InterchangeEngine are Source/Runtime modules present on every
	/// supported engine from 5.3 up, so this family lights up on all six versions;
	/// the async (5.5+) and reimport (5.6+) entry points self-gate per engine.
	void RegisterInterchangeTools(FSololmcpToolRegistry& Registry);
	/// Interchange batch 2 (Layer C node-graph reflection, 11 tools). One generic
	/// get/set pair reaches the ~877 GetCustomXxx/SetCustomXxx accessors spread
	/// across the node classes, and the _batch variant collapses N writes into a
	/// single game-thread entry for queued workloads.
	void RegisterInterchangeNodeTools(FSololmcpToolRegistry& Registry);
	/// Interchange batch 3 (Layer B pipeline configuration, 5 tools). Pipeline
	/// surface is UPROPERTY rather than UFUNCTION, so writes go through generic
	/// property reflection with readback receipts; the _batch variant reconfigures
	/// many pipelines in one game-thread entry.
	void RegisterInterchangePipelineTools(FSololmcpToolRegistry& Registry);
	/// Interchange batch 4 (Layer D production + Layer E diagnostics, 5 tools).
	/// Directory scan, bulk import, import-data audit, source relink and receipt
	/// validation — each handling N files per game-thread entry.
	void RegisterInterchangeBatchTools(FSololmcpToolRegistry& Registry);
	/// ControlRig hierarchy transforms and element reflection (5 tools). URigHierarchy
	/// is keyed by FRigElementKey throughout, so one generic transform get/set pair
	/// reaches its ~148 callables; the _batch variants pose many elements in one
	/// game-thread entry under a single undo step.
	void RegisterRigHierarchyTools(FSololmcpToolRegistry& Registry);
	/// RigVM graph reflection and batch editing (5 tools). URigVMPin::GetPinPath
	/// returns the same address SetPinDefaultValue and AddLink accept, so reading a
	/// graph yields directly writable addresses; rigvm_node_catalog moves unit-node
	/// struct validation from execution time to planning time.
	void RegisterRigVMGraphTools(FSololmcpToolRegistry& Registry);
	/// ControlRig x Sequencer integration (4 tools). Covers the JSON-addressable part
	/// of UControlRigSequencerEditorLibrary; much of that library is keyed by live
	/// editor objects (UMovieSceneSection, UTickableConstraint, FMovieSceneBindingProxy)
	/// with no stable address outside an open Sequencer.
	void RegisterRigSequencerTools(FSololmcpToolRegistry& Registry);
	/// Session handles for live editor objects (3 tools). Large parts of the editor
	/// API are keyed by objects with no asset path — movie scene sections, tickable
	/// constraints, transient graphs — and are unreachable over the wire without a
	/// handle. Minting lives in the owning tool families; only inspection and
	/// release are generic. See SololmcpObjectHandles.h.
	void RegisterObjectHandleTools(FSololmcpToolRegistry& Registry);
	/// GeometryScripting dynamic mesh sessions (3 tools). Every one of that library's
	/// ~509 callables acts on a UDynamicMesh, a transient object with no asset path;
	/// holding one by session handle turns per-operation asset round trips into
	/// open / operate N times / save.
	/// Bounded tool discovery (1 tool). An unfiltered tools/list here is ~4.1M
	/// tokens; this makes the whole surface discoverable inside a context budget,
	/// with every listing reporting whether it is complete.
	void RegisterToolCatalogTools(FSololmcpToolRegistry& Registry);
	void RegisterGeometryScriptTools(FSololmcpToolRegistry& Registry);
	/// Reflection dispatch over the GeometryScripting function libraries (3 tools).
	/// Every operation in those libraries is a UFUNCTION, so one dispatcher plus a
	/// catalog reaches the whole surface and picks up new operations per engine
	/// version without a code change.
	void RegisterGeometryOpTools(FSololmcpToolRegistry& Registry);
	/// Reflection dispatch over seven large editor API domains (4 tools):
	/// DynamicMaterial, ClonerEffector, USD, Datasmith, Sequencer, MovieRenderQueue
	/// and TakeRecorder — 2,585 BlueprintCallable functions on 5.8. Purely
	/// reflective: no module dependency, so a disabled plugin reports itself absent
	/// instead of breaking the build, and the same binary serves 5.3 through 5.8.
void RegisterEditorApiTools(FSololmcpToolRegistry& Registry);
	/// Native UE editor ABrush/UModel/FPoly authoring, CSG, inspection, rebuild and conversion.
	void RegisterNativeBspTools(FSololmcpToolRegistry& Registry);
}
