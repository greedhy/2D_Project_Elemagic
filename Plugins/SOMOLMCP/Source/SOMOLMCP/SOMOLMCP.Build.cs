// Copyright 2026 SOMOLAGENT. All Rights Reserved.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnrealBuildTool;

public class SOMOLMCP : ModuleRules
{
	/// Every *.Build.cs basename reachable from the target engine, built once per
	/// UBT process. Engine roots differ by version far more than the docs suggest
	/// (UE 5.3 ships 1696 modules, UE 5.7 ships 2337), so availability is measured
	/// from disk instead of compared against a hardcoded MinorVersion.
	private static HashSet<string> EngineModuleIndexCache;
	private static string EngineModuleIndexRoot;
	private static readonly object EngineModuleIndexLock = new object();

	private static readonly string[] EngineModuleScanRoots = { "Source", "Plugins" };
	private static readonly string[] EngineModuleScanSkipDirs =
	{
		"Intermediate", "Binaries", "Saved", "DerivedDataCache", "Content",
	};

	private static HashSet<string> GetEngineModuleIndex(string EngineDir)
	{
		lock (EngineModuleIndexLock)
		{
			if (EngineModuleIndexCache != null && EngineModuleIndexRoot == EngineDir)
			{
				return EngineModuleIndexCache;
			}

			HashSet<string> Index = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
			foreach (string ScanRoot in EngineModuleScanRoots)
			{
				string Root = Path.Combine(EngineDir, ScanRoot);
				if (!Directory.Exists(Root))
				{
					continue;
				}

				Stack<string> Pending = new Stack<string>();
				Pending.Push(Root);
				while (Pending.Count > 0)
				{
					string Current = Pending.Pop();
					try
					{
						foreach (string Child in Directory.EnumerateDirectories(Current))
						{
							string Leaf = Path.GetFileName(Child);
							if (Array.IndexOf(EngineModuleScanSkipDirs, Leaf) < 0)
							{
								Pending.Push(Child);
							}
						}
						foreach (string File in Directory.EnumerateFiles(Current, "*.Build.cs"))
						{
							string Leaf = Path.GetFileName(File);
							Index.Add(Leaf.Substring(0, Leaf.Length - ".Build.cs".Length));
						}
					}
					catch (UnauthorizedAccessException)
					{
						// Unreadable engine subtree: treat as contributing no modules.
					}
					catch (IOException)
					{
					}
				}
			}

			EngineModuleIndexCache = Index;
			EngineModuleIndexRoot = EngineDir;
			return Index;
		}
	}

	/// True when the module exists in the target engine. Always pair a call with a
	/// SOMOLMCP_HAS_<MODULE> definition so C++ can gate on the same fact.
	private bool EngineModuleExists(string ModuleName)
	{
		return GetEngineModuleIndex(EngineDirectory).Contains(ModuleName);
	}

	/// Add a module only if the target engine actually ships it, and publish the
	/// result as SOMOLMCP_HAS_<MODULE> (1/0) for registration-time gating.
	/// Returns whether the module was added.
	private bool TryAddModule(string ModuleName)
	{
		bool bExists = EngineModuleExists(ModuleName);
		if (bExists)
		{
			PrivateDependencyModuleNames.Add(ModuleName);
		}
		PrivateDefinitions.Add(string.Format("SOMOLMCP_HAS_{0}={1}", ModuleName.ToUpperInvariant(), bExists ? "1" : "0"));
		return bExists;
	}

	/// Add a whole family, and publish SOMOLMCP_HAS_<FAMILY>=1 only when every
	/// member is present. A partially available family is unusable, so it reports 0
	/// and the individual module defines record which pieces were missing.
	private bool TryAddModuleFamily(string FamilyDefine, params string[] ModuleNames)
	{
		bool bComplete = ModuleNames.All(EngineModuleExists);
		foreach (string ModuleName in ModuleNames)
		{
			TryAddModule(ModuleName);
		}
		PrivateDefinitions.Add(string.Format("SOMOLMCP_HAS_{0}={1}", FamilyDefine, bComplete ? "1" : "0"));
		return bComplete;
	}

	public SOMOLMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Needed for SLevelEditor.h (Private header of LevelEditor module)
		PrivateIncludePathModuleNames.AddRange(new string[] { "LevelEditor" });

		// SololmcpDomainTools.cpp is >1MB — disable Unity Build to prevent UBT from
		// splitting it into multiple translation units (which causes LNK2019 on functions
		// defined near the end of the file, e.g. RegisterSequencerAudioVfxTools).
		bUseUnity = false;

		// UE 5.8 + MSVC 14.44 can hit C1001 in p2 optimization while compiling the
		// very large editor-tool registration units. This plugin is editor automation,
		// so build reliability is more important than optimized runtime codegen.
		OptimizeCode = CodeOptimization.Never;

		// UE 5.3 still uses the bool shrinking argument. UE 5.4+ exposes
		// EAllowShrinking, so keep one source expression across supported engines.
		PrivateDefinitions.Add(Target.Version.MajorVersion == 5 && Target.Version.MinorVersion <= 3
			? "SOMOLMCP_NO_SHRINK=false"
			: "SOMOLMCP_NO_SHRINK=EAllowShrinking::No");

		PrivateDefinitions.Add("SOMOLMCP_WITH_WORLDFORGE=0");
		PrivateDefinitions.Add("SOMOLMCP_WITH_LAYERED_COUPLING=1");

		// Terrain fix batch 0+1 (2026-07-21): proof schema v1 compatibility
		// window. Set to 0 to reject legacy v1 terrain constraint proofs.
		PrivateDefinitions.Add("SOMOLMCP_TERRAIN_PROOF_V1_COMPAT=1");

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealEd",
				"LevelEditor",
				"MessageLog",
				"AssetTools",
				"AssetRegistry",
				"EditorSubsystem",
				"Projects",
				"Json",
				"JsonUtilities",
				"HTTP",
				"HTTPServer",
				"Sockets",
				"Networking",
				"Foliage",
				"Landscape",
				"LandscapeEditor",
				"BlueprintEditorLibrary",
				"BlueprintGraph",
				"Kismet",
				"MaterialEditor",
				"StaticMeshEditor",
				"SkeletalMeshEditor",
				"MeshBoneReduction",
				"AnimationBlueprintLibrary",
				"AnimGraph",
				"AnimationBlueprintEditor",
				"AudioEditor",
				"MetasoundEngine",
				"MetasoundFrontend",
				"MetasoundEditor",
				"DataLayerEditor",
				"MovieScene",
				"MovieSceneTracks",
				"MovieSceneTools",
				"LevelSequence",
				"LevelSequenceEditor",
				"Sequencer",
				"SequencerScriptingEditor",
				"CinematicCamera",
				"UMG",
				"UMGEditor",
				"Niagara",
				"NiagaraCore",
				"NiagaraEditor",
				"ControlRig",
				"ControlRigDeveloper",
				"ControlRigEditor",
				"RigVM",
				"RigVMDeveloper",
				"RigVMEditor",
				"WorldPartitionEditor",
				// v1.6.0 新增模块
				"PCG",
				"PCGEditor",
				// v1.7.0 新增模块
				"EnhancedInput",
				"AIModule",
				"GameplayTasks",
				"NavigationSystem",
				"Navmesh",
				// v1.7.0 Tier 2-4 新增模块
				"AIGraph",
				"IKRig",
				"IKRigEditor",
				// GameplayAbilities/GameplayTags/GameplayTagsEditor — re-enabled for UE 5.7.4
				"GameplayAbilities",
				"GameplayTags",
				"GameplayTagsEditor",
				"GeometryCollectionEngine",
				// v1.8.0 截图与调试工具新增模块
				"ImageWrapper",       // IImageWrapperModule - PNG 压缩
				"RHI",                // Shader platform and feature-level readback
				"RenderCore",         // GPU readback utilities
				"SlateRHIRenderer",   // FWidgetRenderer - Slate 窗口渲染到纹理
				"Slate",
				"SlateCore",
				// v1.8.0 Editor UI 自动化工具新增模块
				"InputCore",              // FKey, FKeyEvent, FPointerEvent
				"ToolMenus",              // 菜单项注册与执行
				"ContentBrowserData",     // IContentBrowserSingleton
				"ContentBrowser",         // Content Browser navigation
				"CollectionManager",      // P2 concrete asset collection tools
				"Blutility",              // P2 concrete Editor Utility safe-run tools
				// LandscapeEditorUtils.h 已包含在 LandscapeEditor 模块中（第 33 行）
				"AppFramework",           // FSlateApplication helpers
				// v3.1.0 TextureCompressor for ITextureCompressorModule
				"TextureCompressor",
				// UE 5.7 LNK2019 修复 — 新增模块依赖
				"PhysicsCore",        // StaticEnum<ECollisionTraceFlag>
				"Chaos",              // FManagedArrayCollection, FTransformCollection
				"ApplicationCore",    // IPlatformInputDeviceMapper
				"EditorFramework",    // FBuiltinEditorModes
				// v3.6.0 Media & Ingest tools (Stage-13)
				"MediaAssets",        // UFileMediaSource, UStreamMediaSource, UMediaPlayer, UMediaTexture, UMediaPlaylist
				"MediaUtils",         // FMediaPlayerFacade helpers
				"ImgMedia",           // UImgMediaSource
				"EditorScriptingUtilities", // UEditorAssetLibrary — MediaIngestTools
				// v3.10.0 P2-1 PhysicsAsset tools — FPhysicsAssetUtils::CreateFromSkeletalMesh
				"PhysicsUtilities",
				"ProceduralMeshComponent",
				// v3.12.0 P4-3 XR — UMotionControllerComponent + IXRTrackingSystem
				"HeadMountedDisplay",
			}
		);

		// Production video automation: native Movie Render Queue/Graph execution,
		// MP4 output validation, and Win64 desktop capture encoding.
		//
		// MovieRenderPipelineMP4Encoder only exists from UE 5.6 on. Adding it
		// unconditionally made UBT fail module resolution on UE 5.3/5.4/5.5 before
		// compiling anything, so those engines never got a buildable plugin at all.
		// The base MRQ modules are present on every supported engine; only the MP4
		// encoder is gated, and SOMOLMCP_HAS_MRQ_MP4 tells the video tools whether
		// native MP4 output is available or they must fall back to image sequences.
		TryAddModuleFamily(
			"MRQ",
			"MovieRenderPipelineCore",
			"MovieRenderPipelineEditor",
			"MovieRenderPipelineSettings",
			"MovieRenderPipelineRenderPasses"
		);
		PrivateDefinitions.Add(TryAddModule("MovieRenderPipelineMP4Encoder")
			? "SOMOLMCP_HAS_MRQ_MP4=1"
			: "SOMOLMCP_HAS_MRQ_MP4=0");

		// UE 5.3 keeps ControlRigBlueprintFactory.h in ControlRigEditor/Private; it
		// moved to Public in 5.4. The header does ship with Installed builds, so
		// adding the private include path on 5.3 keeps one code path across all
		// engines instead of stubbing out the file that declares it — which is what
		// used to cost UE 5.3 all 623 tools in SololmcpDomainTools.cpp.
		if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion <= 3)
		{
			PrivateIncludePaths.Add(Path.Combine(
				EngineDirectory, "Plugins/Animation/ControlRig/Source/ControlRigEditor/Private"));
		}

		// Interchange import/export framework. InterchangeCore and InterchangeEngine
		// are Source/Runtime modules shipped by every engine from 5.3 up, including
		// Installed/Rocket builds, so the tool family reaches all supported versions.
		// Routed through TryAddModule anyway: a stripped engine layout then yields
		// SOMOLMCP_HAS_INTERCHANGEENGINE=0 and typed capability answers, instead of
		// failing UBT module resolution the way the MRQ block used to.
		TryAddModuleFamily("INTERCHANGE", "InterchangeCore", "InterchangeEngine");

		// GeometryScripting ships as a Runtime plugin on every engine from 5.3 up, in
		// Installed builds too. The Editor half carries asset creation and OpenSubdiv,
		// which the reflection dispatcher would otherwise never see: its catalog only
		// finds classes whose module is actually linked, so an unlinked module reads
		// as "this operation does not exist" rather than as a missing dependency.
		// GeometryFramework owns UDynamicMesh itself; GeometryScripting only operates
		// on it. On 5.8 it linked anyway through a transitive dependency, so the
		// omission was invisible until 5.7, where it surfaced as an unresolved
		// Z_Construct_UClass_UDynamicMesh_NoRegister at link time -- every
		// translation unit compiled clean. Naming it explicitly is correct on every
		// version and does not rely on another module's dependency list.
		// PCG ships on every supported engine. Detected rather than assumed so the
		// native PCG tools key off "is PCG here" instead of riding on an unrelated
		// flag: they were gated behind SOMOLMCP_WITH_UE58_MESHPARTITION, which cost
		// all 41 of them on 5.7 even though the file does not reference MeshPartition
		// once.
		TryAddModule("PCG");
		// StructUtils is a separate Experimental plugin module on 5.3 and was folded into
		// CoreUObject from 5.4. Without it the PCG property-bag calls compile but fail to
		// link there, so it is added when present rather than assumed either way.
		TryAddModule("StructUtils");

		TryAddModuleFamily("GEOMETRYSCRIPTING",
			"GeometryScriptingCore", "GeometryScriptingEditor", "GeometryFramework");
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");

		if (Target.bBuildEditor)
		{
			// Availability, not version number.
			//
			// This list used to sit behind `MinorVersion >= 8`, which meant every tool
			// depending on any of it compiled out on 5.7 -- 687 tools, measured by
			// diffing the two standalone builds tool-for-tool. Only 11 of these
			// modules are genuinely 5.8-only (the MeshPartition family,
			// ControlRigDynamics and the DirectMeshControl pair); the other 18 ship on
			// 5.7 too and were being refused for no reason other than the branch they
			// were written in.
			//
			// TryAddModule asks the engine instead of asking the version: present
			// modules are added on every engine that has them, and absent ones publish
			// SOMOLMCP_HAS_<MODULE>=0 so their tools stay compiled out with a typed
			// capability answer rather than a link error.
			foreach (string EditorModule in new string[]
				{
					"DataflowCore",
					"FractureEngine",
					"MeshPaintingToolset",
					"MeshPartition",
					"MeshPartitionEditor",
					"MeshTerrainMode",
					"MeshPartitionModelingToolset",
					"MeshPartitionEditorUI",
					"MeshPartitionWater",
					"PCGMeshPartitionInterop",
					"PCGMeshPartitionInteropEditor",
					"ProceduralVegetation",
					"ProceduralVegetationEditor",
					"DynamicMesh",
					"GeometryCore",
					"GeometryFramework",
					"InteractiveToolsFramework",
					"MeshConversion",
					"MeshDescription",
					"ModelingComponents",
					"StaticMeshDescription",
					"Water",
					"WaterEditor",
					"DataHierarchyEditor",
					"StructUtilsEditor",
					"ControlRigPhysics",
					"ControlRigDynamics",
					"DirectMeshControl",
					"DirectMeshControlRig",
					// GeometryScriptingCore is handled by the TryAddModule block above.
				})
			{
				TryAddModule(EditorModule);
			}

			PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source/Runtime/Engine/Internal"));

			// These two feature defines must follow the modules, not the branch they
			// used to live in. Setting MESHPARTITION=1 on an engine without the
			// MeshPartition module would compile its tools in and then fail to link --
			// the same class of defect as the GeometryFramework omission, just in the
			// other direction.
			bool bHasMeshPartition = EngineModuleExists("MeshPartition")
				&& EngineModuleExists("MeshPartitionEditor")
				&& EngineModuleExists("MeshTerrainMode");
			PrivateDefinitions.Add(bHasMeshPartition
				? "SOMOLMCP_WITH_UE58_MESHPARTITION=1"
				: "SOMOLMCP_WITH_UE58_MESHPARTITION=0");
			if (bHasMeshPartition)
			{
				PrivateIncludePaths.Add(Path.Combine(EngineDirectory,
					"Plugins/Experimental/MeshTerrainMode/Source/MeshTerrainMode/Private"));
			}

			// The PVE surface reaches WorldForge suite headers, so it stays tied to the
			// 5.8 branch that ships them rather than to module detection.
			PrivateDefinitions.Add(
				Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 8
					? "SOMOLMCP_WITH_UE58_PVE=1"
					: "SOMOLMCP_WITH_UE58_PVE=0");
		}
		else
		{
			PrivateDefinitions.Add("SOMOLMCP_WITH_UE58_MESHPARTITION=0");
			PrivateDefinitions.Add("SOMOLMCP_WITH_UE58_PVE=0");
		}

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDefinitions.Add("SOMOLMCP_PLATFORM_WIN64=1");
			PublicSystemLibraries.AddRange(new string[]
			{
				"mfplat.lib",
				"mfreadwrite.lib",
				"mfuuid.lib",
				"ole32.lib",
				"gdi32.lib",
				"user32.lib",
			});
		}
	}
}
