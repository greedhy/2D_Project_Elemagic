// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Services/SololmcpEditorServices.h"

#include "Services/SololmcpAssetLocks.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetToolsModule.h"
#include "Containers/StringConv.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "Animation/Skeleton.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Features/IModularFeatures.h"
#include "HAL/PlatformCrt.h"
#include "MaterialEditingLibrary.h"
#include "Misc/OutputDevice.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 6
#include "Containers/UnrealString.h"
#else
#include "Misc/StringOutputDevice.h"
#endif
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
// Round 8 fix: only need UObjectHash + Package (UMetaData not needed when we use IsAsset filter)
#include "UObject/UObjectHash.h"
#include "UObject/Package.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "LevelEditorSubsystem.h"
#include "Subsystems/UnrealEditorSubsystem.h"
#include "Toolkits/FConsoleCommandExecutor.h"
#include "Factories/Factory.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Field.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

// Screenshot capture includes
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#include "ImageUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPEditorServices, Log, All);

namespace UE::SOMOLMCP
{
	FCriticalSection& GetFoliageActorMutex()
	{
		static FCriticalSection Mutex;
		return Mutex;
	}

	bool IsFoliageActorTool(const FString& ToolName)
	{
		return ToolName.StartsWith(TEXT("foliage_"));
	}

	// Thread-local re-entrancy flag: a foliage tool that chains another foliage tool
	// (e.g. through a data-driven macro) must not self-deadlock on the mutex.
	// The flag is also what makes UnlockFoliageActorMutex callable from a SEH
	// __except handler after a crash on the owning thread.
	static thread_local bool GThreadHoldsFoliageMutex = false;

	void LockFoliageActorMutex()
	{
		if (GThreadHoldsFoliageMutex)
		{
			return;
		}
		GetFoliageActorMutex().Lock();
		GThreadHoldsFoliageMutex = true;
	}

	void UnlockFoliageActorMutex()
	{
		if (!GThreadHoldsFoliageMutex)
		{
			return;
		}
		GThreadHoldsFoliageMutex = false;
		GetFoliageActorMutex().Unlock();
	}

	bool FSololmcpEditorServices::IsPythonAvailable(FString* OutReason) const
	{
		if (OutReason)
		{
			*OutReason = TEXT("SOMOLMCP is a native C++ queue-only plugin; Python execution is disabled.");
		}
		return false;
	}

	bool FSololmcpEditorServices::ExecutePython(const FString&, const FString&, bool, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) const
	{
		OutStructured->SetBoolField(TEXT("ok"), false);
		OutStructured->SetStringField(TEXT("reason_code"), TEXT("python_execution_disabled"));
		OutStructured->SetStringField(TEXT("execution_mode"), TEXT("queue_only_native_cpp"));
		OutSummary = TEXT("Python execution is disabled.");
		OutError = TEXT("SOMOLMCP accepts native C++ named tools through the job queue; Python scripts are not executed.");
		return false;
	}

	bool FSololmcpEditorServices::ExecuteConsole(const FString& Command, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) const
	{
		TArray<IConsoleCommandExecutor*> CommandExecutors = IModularFeatures::Get().GetModularFeatureImplementations<IConsoleCommandExecutor>(TEXT("ConsoleCommandExecutor"));
		for (IConsoleCommandExecutor* CommandExecutor : CommandExecutors)
		{
			FStringOutputDevice Output;
			Output.SetAutoEmitLineTerminator(true);

			GLog->AddOutputDevice(&Output);
			const bool bSuccess = CommandExecutor->Exec(*Command);
			GLog->RemoveOutputDevice(&Output);

			if (bSuccess)
			{
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutStructured->SetStringField(TEXT("executor"), CommandExecutor->GetName().ToString());
				OutStructured->SetStringField(TEXT("output"), Output);
				OutSummary = TEXT("Console command executed.");
				return true;
			}
		}

		OutError = TEXT("Command failed or was not recognized by any console executor.");
		return false;
	}

	UWorld* FSololmcpEditorServices::ResolveActiveWorld(FString& OutError) const
	{
		// Runtime authority first: any live PIE/game world context wins.
		if (GEngine)
		{
			for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
			{
				UWorld* World = WorldContext.World();
				if (World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game))
				{
					return World;
				}
			}
		}

		// Editor fallback: the editor world hosts WorldSubsystems too, so ERP tools
		// stay fully functional while the editor is idle.
		if (GEditor)
		{
			if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
			{
				return EditorWorld;
			}
		}

		OutError = TEXT("No active PIE/game world and no editor world context is available.");
		return nullptr;
	}

	UWorld* FSololmcpEditorServices::ResolveGameWorld(FString& OutError) const
	{
		if (GEngine)
		{
			for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
			{
				UWorld* World = WorldContext.World();
				if (World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game))
				{
					return World;
				}
			}
		}

		OutError = TEXT("No live PIE/game world context is available.");
		return nullptr;
	}

	UWorld* FSololmcpEditorServices::GetEditorWorld(FString& OutError) const
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is not available.");
			return nullptr;
		}

		// Primary: UnrealEditorSubsystem
		if (UUnrealEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UUnrealEditorSubsystem>())
		{
			if (UWorld* World = Subsystem->GetEditorWorld())
			{
				return World;
			}
		}

		// Fallback: direct editor world context (works when Subsystem returns null)
		if (GEditor->GetEditorWorldContext().World())
		{
			return GEditor->GetEditorWorldContext().World();
		}

		// Fallback: iterate world contexts for any valid editor world
		for (const FWorldContext& Ctx : GEditor->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::Editor && Ctx.World())
			{
				return Ctx.World();
			}
		}

		OutError = TEXT("Unable to resolve editor world.");
		return nullptr;
	}

	UEditorActorSubsystem* FSololmcpEditorServices::GetActorSubsystem(FString& OutError) const
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is not available.");
			return nullptr;
		}

		if (UEditorActorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
		{
			return Subsystem;
		}

		OutError = TEXT("EditorActorSubsystem is not available.");
		return nullptr;
	}

	UEditorAssetSubsystem* FSololmcpEditorServices::GetAssetSubsystem(FString& OutError) const
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is not available.");
			return nullptr;
		}

		if (UEditorAssetSubsystem* Subsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
		{
			return Subsystem;
		}

		OutError = TEXT("EditorAssetSubsystem is not available.");
		return nullptr;
	}

	ULevelEditorSubsystem* FSololmcpEditorServices::GetLevelEditorSubsystem(FString& OutError) const
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is not available.");
			return nullptr;
		}

		if (ULevelEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
		{
			return Subsystem;
		}

		OutError = TEXT("LevelEditorSubsystem is not available.");
		return nullptr;
	}

	UAssetEditorSubsystem* FSololmcpEditorServices::GetAssetEditorSubsystem(FString& OutError) const
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is not available.");
			return nullptr;
		}

		if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			return Subsystem;
		}

		OutError = TEXT("AssetEditorSubsystem is not available.");
		return nullptr;
	}

	UObject* FSololmcpEditorServices::LoadAsset(const FString& AssetPath, FString& OutError) const
	{
		// Phase 3: shared read lock. Multiple parallel loads of the same asset are OK,
		// but a concurrent write (delete/save/create) on the same path will block here
		// until it finishes — and vice versa.
		FAssetReadScope LockScope(AssetPath);
		if (!LockScope.bAcquired)
		{
			OutError = FString::Printf(TEXT("asset_lock_timeout: %s"), *AssetPath);
			return nullptr;
		}

		// Round 8 fix (Apr 2026): the fast path below uses FindObject which, given a
		// PACKAGE path like "/Game/Foo/Bar", returns the UPackage itself, NOT the
		// inner UBlueprint/UMaterial/etc. Cast<UBlueprint>(UPackage) → nullptr →
		// downstream tools fail with "Asset is not a Blueprint." even though the
		// asset was created/saved correctly. Pre-round-7 this didn't bite because
		// the package wasn't kept resident, so FindObject failed and the
		// EditorAssetSubsystem (next branch) auto-completed the path. Round 7's
		// SaveAsset+AssetCreated keeps the package resident → exposed the latent
		// FindObject ambiguity. Fix: normalize package path → object path before
		// FindObject by appending ".AssetName" when no '.' is present, AND if we
		// still get a UPackage back (e.g. fully-qualified path of an external file),
		// peek inside for the primary asset.
		FString NormalizedPath = AssetPath;
		if (!NormalizedPath.IsEmpty() && !NormalizedPath.Contains(TEXT(".")))
		{
			FString PkgRoot, AssetLeaf;
			if (NormalizedPath.Split(TEXT("/"), &PkgRoot, &AssetLeaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !AssetLeaf.IsEmpty())
			{
				NormalizedPath = NormalizedPath + TEXT(".") + AssetLeaf;
			}
		}

		// Fast path: check if the asset is already in memory (e.g. just created via CreateAsset
		// but not yet saved to disk).  FindObject avoids a pointless disk round-trip and fixes
		// the majority of LOAD_FAIL errors where tests create-then-immediately-load an asset.
		if (UObject* Found = FindObject<UObject>(nullptr, *NormalizedPath))
		{
			// Defensive: if NormalizedPath lookup somehow still returned a UPackage,
			// dig out the primary asset inside it instead of handing the package
			// back to a Cast<UBlueprint> caller that would fail.
			if (UPackage* AsPkg = Cast<UPackage>(Found))
			{
				UObject* Inner = nullptr;
				// IsAsset() is the canonical "this UObject is the primary asset of
				// its package" check — filters out UMetaData / transients / etc.
				// No new include needed (declared in UObject base class).
				ForEachObjectWithOuter(AsPkg, [&Inner](UObject* It)
				{
					if (Inner) return;
					if (It && It->IsAsset())
					{
						Inner = It;
					}
				}, /*bIncludeNestedObjects=*/false);
				if (Inner) { return Inner; }
			}
			return Found;
		}

		UEditorAssetSubsystem* Subsystem = GetAssetSubsystem(OutError);
		if (!Subsystem)
		{
			return nullptr;
		}

		// Subsystem->LoadAsset accepts both package- and object-form paths; pass the
		// original (unnormalized) so it can do its own resolution if upstream needs it.
		if (UObject* Object = Subsystem->LoadAsset(AssetPath))
		{
			return Object;
		}

		// Fallback: direct LoadObject for engine built-in assets or packages that the
		// EditorAssetSubsystem cannot resolve (e.g. /Engine/ built-in shapes).
		if (UObject* Loaded = LoadObject<UObject>(nullptr, *AssetPath))
		{
			return Loaded;
		}

		OutError = FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath);
		return nullptr;
	}

	UClass* FSololmcpEditorServices::ResolveClass(const FString& ClassPath, FString& OutError) const
	{
		if (ClassPath.IsEmpty())
		{
			OutError = TEXT("Class path is empty.");
			return nullptr;
		}

		if (UClass* ExistingClass = FindObject<UClass>(nullptr, *ClassPath))
		{
			return ExistingClass;
		}

		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return LoadedClass;
		}

		OutError = FString::Printf(TEXT("Failed to resolve class: %s"), *ClassPath);
		return nullptr;
	}

	AActor* FSololmcpEditorServices::FindActorByLabelOrName(const FString& ActorId, FString& OutError) const
	{
		UWorld* World = GetEditorWorld(OutError);
		if (!World)
		{
			return nullptr;
		}

		// FIXED #13: 按精确度分三轮优先级查找，避免 label 不唯一时返回错误 Actor。
		// 优先级：PathName（全局唯一）> Name（同 World 内唯一）> Label（可能重复）

		// 第一轮：按 PathName 精确匹配（最准确）
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) { continue; }
			if (Actor->GetPathName() == ActorId)
			{
				return Actor;
			}
		}

		// 第二轮：按 FName（GetName）精确匹配（同 World 内唯一）
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) { continue; }
			if (Actor->GetName() == ActorId)
			{
				return Actor;
			}
		}

		// 第三轮：按 ActorLabel 匹配（可能不唯一，返回第一个）
		AActor* LabelMatch = nullptr;
		int32 LabelMatchCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) { continue; }
			if (Actor->GetActorLabel() == ActorId)
			{
				if (!LabelMatch)
				{
					LabelMatch = Actor;
				}
				++LabelMatchCount;
			}
		}

		if (LabelMatch)
		{
			if (LabelMatchCount > 1)
			{
				UE_LOG(LogSOMOLMCPEditorServices, Warning,
					TEXT("FindActorByLabelOrName: Found %d actors with label '%s', returning first match. Use PathName for unambiguous lookup."),
					LabelMatchCount, *ActorId);
			}
			return LabelMatch;
		}

		// 第四轮（singleton-by-type 兜底）：agent 经常用「类型名」指代场景里唯一的单例
		// （如 "SkyLight" / "DirectionalLight" / "SkyAtmosphere"），而 label 会被引擎
		// 自动去重（SkyLight2）或在工具重建该 actor 后改变，导致前三轮都 miss。若
		// ActorId 等于某个类名且该类**恰好只有一个**实例，则返回它；若有多个，返回
		// 列出唯一名的歧义错误，让调用方用 GetName()/path 精确指定（绝不对重复类型瞎猜）。
		{
			AActor* ClassMatch = nullptr;
			int32 ClassMatchCount = 0;
			TArray<FString> ClassMatchNames;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor) { continue; }
				const UClass* Cls = Actor->GetClass();
				if (!Cls) { continue; }
				const FString ClsName = Cls->GetName();
				if (ClsName.Equals(ActorId, ESearchCase::IgnoreCase)
					|| (ActorId.StartsWith(TEXT("A")) && ClsName.Equals(ActorId.Mid(1), ESearchCase::IgnoreCase)))
				{
					if (!ClassMatch) { ClassMatch = Actor; }
					++ClassMatchCount;
					if (ClassMatchNames.Num() < 8) { ClassMatchNames.Add(Actor->GetName()); }
				}
			}
			if (ClassMatchCount == 1)
			{
				return ClassMatch;
			}
			if (ClassMatchCount > 1)
			{
				OutError = FString::Printf(
					TEXT("Ambiguous actor type '%s': %d actors of this class exist (%s). Pass a unique actor name or path instead."),
					*ActorId, ClassMatchCount, *FString::Join(ClassMatchNames, TEXT(", ")));
				return nullptr;
			}
		}

		OutError = FString::Printf(TEXT("Failed to find actor: %s"), *ActorId);
		return nullptr;
	}

	UObject* FSololmcpEditorServices::CreateAsset(const FString& PackagePath, const FString& AssetName, const FString& AssetClassPath, const FString& FactoryClassPath, const TSharedPtr<FJsonObject>& FactoryOverrides, FString& OutError, bool bReplaceExisting) const
	{
		// Phase 3: exclusive write lock for the target asset path. Blocks concurrent
		// load/save/delete on the SAME path (different paths run fully in parallel).
		const FString FullAssetPath = PackagePath / AssetName;
		FAssetWriteScope LockScope(FullAssetPath);
		if (!LockScope.bAcquired)
		{
			OutError = FString::Printf(TEXT("asset_lock_timeout: %s"), *FullAssetPath);
			return nullptr;
		}

		FString ClassError;
		UClass* AssetClass = ResolveClass(AssetClassPath, ClassError);
		if (!AssetClass)
		{
			OutError = ClassError;
			return nullptr;
		}

		// Phase 3 hardening: prevent UE UObjectGlobals.cpp:3673 check() collision.
		// If something exists at this path AND it's a different class than what we're
		// about to create, refuse cleanly. UE's check() is long-jump fatal — SEH can't catch.
		if (UObject* ExistingObj = FindObject<UObject>(nullptr, *FullAssetPath))
		{
			// Check inside the UPackage for the actual asset
			if (UPackage* ExistingPkg = Cast<UPackage>(ExistingObj))
			{
				UObject* ExistingAsset = nullptr;
				ForEachObjectWithOuter(ExistingPkg, [&ExistingAsset](UObject* Inner)
				{
					if (!ExistingAsset && Inner && !Inner->IsA<UPackage>() && Inner->IsAsset())
					{
						ExistingAsset = Inner;
					}
				}, /*bIncludeNestedObjects*/ false);
				if (ExistingAsset && ExistingAsset->GetClass() != AssetClass)
				{
					OutError = FString::Printf(
						TEXT("class_collision: '%s' exists as %s, refusing to replace with %s. Delete the existing asset first or use a different path."),
						*FullAssetPath, *ExistingAsset->GetClass()->GetName(), *AssetClass->GetName());
					UE_LOG(LogSOMOLMCPEditorServices, Warning, TEXT("SOMOLMCP: refused class-collision create: %s"), *OutError);
					return nullptr;
				}
			}
			else if (ExistingObj->GetClass() != AssetClass)
			{
				// Direct asset object found (not wrapped in UPackage) — same check
				OutError = FString::Printf(
					TEXT("class_collision: '%s' exists as %s, refusing to replace with %s."),
					*FullAssetPath, *ExistingObj->GetClass()->GetName(), *AssetClass->GetName());
				UE_LOG(LogSOMOLMCPEditorServices, Warning, TEXT("SOMOLMCP: refused class-collision create: %s"), *OutError);
				return nullptr;
			}
		}

		UFactory* Factory = CreateFactory(FactoryClassPath, FactoryOverrides, OutError);
		if (!Factory)
		{
			return nullptr;
		}

		// Suppress overwrite confirmation dialog: auto-delete existing asset at target path.
		{
			// Round 12E: check BOTH package-path AND object-path forms because in-memory
			// ghosts (from interrupted prior runs) may live under either.
			const bool bGhostPkg = AssetExists(FullAssetPath);
			const bool bGhostObj = AssetExists(FullAssetPath + TEXT(".") + AssetName);
			if (bGhostPkg || bGhostObj)
			{
				if (!bReplaceExisting)
				{
					OutError = FString::Printf(
						TEXT("asset_exists: '%s' already exists; use a unique asset_name or pass replace_existing=true for safe asset classes."),
						*FullAssetPath);
					UE_LOG(LogSOMOLMCPEditorServices, Warning, TEXT("SOMOLMCP: refused implicit overwrite: %s"), *OutError);
					return nullptr;
				}
				if (AssetClass && AssetClass->GetName().Contains(TEXT("PCGGraph")))
				{
					OutError = FString::Printf(
						TEXT("unsafe_replace_existing: refusing unattended replacement of PCG graph '%s'; use a unique asset path or delete explicitly after dependency/readback checks."),
						*FullAssetPath);
					UE_LOG(LogSOMOLMCPEditorServices, Warning, TEXT("SOMOLMCP: refused PCG graph overwrite: %s"), *OutError);
					return nullptr;
				}
				// Phase 3 NOTE: cannot call our own DeleteAsset() here — it would
				// re-acquire the write lock on the same normalized path and deadlock
				// (FRWLock is non-recursive). Drive the EditorAssetSubsystem directly;
				// we already hold the exclusive lock so this is race-safe.
				FString SubsystemError;
				if (UEditorAssetSubsystem* AssetSub = GetAssetSubsystem(SubsystemError))
				{
					AssetSub->DeleteAsset(FullAssetPath);
				}
				UE_LOG(LogSOMOLMCPEditorServices, Log, TEXT("SOMOLMCP: Auto-deleted existing asset for overwrite: %s (pkg=%d obj=%d)"), *FullAssetPath, bGhostPkg ? 1 : 0, bGhostObj ? 1 : 0);
				// Also try to purge any stale in-memory UPackage that DeleteAsset might leave behind
				if (UPackage* StalePkg = FindObject<UPackage>(nullptr, *FullAssetPath))
				{
					StalePkg->ClearFlags(RF_Standalone);
					StalePkg->RemoveFromRoot();
					UE_LOG(LogSOMOLMCPEditorServices, Log, TEXT("SOMOLMCP: Cleared stale UPackage flags: %s"), *FullAssetPath);
				}
			}
		}

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		UObject* CreatedAsset = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, AssetClass, Factory);
		if (!CreatedAsset)
		{
			// UE 5.8: BlendSpace-family factories (BlendSpace/BlendSpace1D/AimOffset)
			// return nullptr from FactoryCreateNew when TargetSkeleton is unset (their
			// skeleton picker is modal and cannot run in queue mode). Inject the engine's
			// SkeletalCube skeleton (UE 5.8 ships it as a standalone asset
			// SkeletalCube_Skeleton.SkeletalCube_Skeleton; older layouts embedded it as
			// SkeletalCube.Skeleton) so the requested asset class is created
			// deterministically; callers may override via factory_overrides.
			if (FObjectProperty* SkeletonProp = FindFProperty<FObjectProperty>(Factory->GetClass(), TEXT("TargetSkeleton")))
			{
				if (SkeletonProp->GetObjectPropertyValue_InContainer(Factory) == nullptr)
				{
					USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton"));
					if (!Skeleton)
					{
						Skeleton = LoadObject<USkeleton>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.Skeleton"));
					}
					if (Skeleton)
					{
						SkeletonProp->SetObjectPropertyValue_InContainer(Factory, Skeleton);
					}
				}
			}
			// AssetTools::CreateAsset may still decline import-oriented factories (e.g.
			// UTextureFactory); fall back to driving FactoryCreateNew directly on the
			// destination asset package (PackagePath/AssetName, matching AssetTools
			// semantics) so the requested asset class is created deterministically.
			const FString AssetPackagePath = PackagePath / AssetName;
			if (UPackage* TargetPackage = CreatePackage(*AssetPackagePath))
			{
				CreatedAsset = Factory->FactoryCreateNew(AssetClass, TargetPackage, FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn);
			}
			if (!CreatedAsset)
			{
				OutError = FString::Printf(TEXT("Failed to create asset %s in %s"), *AssetName, *PackagePath);
				return nullptr;
			}
			UE_LOG(LogSOMOLMCPEditorServices, Log, TEXT("SOMOLMCP: AssetTools::CreateAsset declined %s at %s; FactoryCreateNew fallback succeeded."), *AssetClass->GetName(), *FullAssetPath);
		}

		// Round 12E: detect UE's silent auto-rename. If the returned asset's name differs
		// from what we asked for, UE collided in-memory and added a suffix (e.g. _01).
		// This breaks every downstream caller using the requested path. Fail loudly so
		// the test/UI can react instead of returning a "success" with a different path.
		const FString ActualName = CreatedAsset->GetName();
		if (ActualName != AssetName)
		{
			OutError = FString::Printf(
				TEXT("CreateAsset auto-renamed: requested '%s' but UE created '%s' (in-memory collision in %s)"),
				*AssetName, *ActualName, *PackagePath);
			UE_LOG(LogSOMOLMCPEditorServices, Warning, TEXT("SOMOLMCP: %s"), *OutError);
			// Best-effort: delete the auto-renamed asset so we don't leak it
			FString DelErr;
			DeleteAsset(PackagePath / ActualName, DelErr);
			return nullptr;
		}

		return CreatedAsset;
	}

	bool FSololmcpEditorServices::SaveAsset(const FString& AssetPath, bool bOnlyIfDirty, FString& OutError) const
	{
		// Phase 3: exclusive write lock — save mutates the package file on disk.
		FAssetWriteScope LockScope(AssetPath);
		if (!LockScope.bAcquired)
		{
			OutError = FString::Printf(TEXT("asset_lock_timeout: %s"), *AssetPath);
			return false;
		}

		UEditorAssetSubsystem* Subsystem = GetAssetSubsystem(OutError);
		if (!Subsystem)
		{
			return false;
		}

		if (!Subsystem->SaveAsset(AssetPath, bOnlyIfDirty))
		{
			OutError = FString::Printf(TEXT("Failed to save asset: %s"), *AssetPath);
			return false;
		}
		return true;
	}

	bool FSololmcpEditorServices::DeleteAsset(const FString& AssetPath, FString& OutError) const
	{
		// Phase 3: exclusive write lock — delete is destructive and races with
		// concurrent loads/saves can crash the editor (e.g. ForceDeleteObjects ensure).
		FAssetWriteScope LockScope(AssetPath);
		if (!LockScope.bAcquired)
		{
			OutError = FString::Printf(TEXT("asset_lock_timeout: %s"), *AssetPath);
			return false;
		}

		UEditorAssetSubsystem* Subsystem = GetAssetSubsystem(OutError);
		if (!Subsystem)
		{
			return false;
		}

		const bool bDisposableAsset = AssetPath.StartsWith(TEXT("/Game/SOMOLMCP/Disposable"));
		auto ResolveAssetObjectWithoutServiceLock = [&](bool bAllowLoad) -> UObject*
		{
			FString NormalizedPath = AssetPath;
			if (!NormalizedPath.IsEmpty() && !NormalizedPath.Contains(TEXT(".")))
			{
				FString PkgRoot;
				FString AssetLeaf;
				if (NormalizedPath.Split(TEXT("/"), &PkgRoot, &AssetLeaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !AssetLeaf.IsEmpty())
				{
					NormalizedPath = NormalizedPath + TEXT(".") + AssetLeaf;
				}
			}

			if (UObject* Found = FindObject<UObject>(nullptr, *NormalizedPath))
			{
				if (UPackage* AsPkg = Cast<UPackage>(Found))
				{
					UObject* Inner = nullptr;
					ForEachObjectWithOuter(AsPkg, [&Inner](UObject* It)
					{
						if (Inner) { return; }
						if (It && It->IsAsset())
						{
							Inner = It;
						}
					}, /*bIncludeNestedObjects=*/false);
					if (Inner)
					{
						return Inner;
					}
				}
				else
				{
					return Found;
				}
			}

			return bAllowLoad ? Subsystem->LoadAsset(AssetPath) : nullptr;
		};

		if (bDisposableAsset)
		{
			if (UObject* AssetObject = ResolveAssetObjectWithoutServiceLock(false))
			{
				FString AssetEditorError;
				if (UAssetEditorSubsystem* AssetEditor = GetAssetEditorSubsystem(AssetEditorError))
				{
					AssetEditor->CloseAllEditorsForAsset(AssetObject);
				}
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
			if (UObject* StillLoadedAsset = ResolveAssetObjectWithoutServiceLock(false))
			{
				const FString LoadedClassName = StillLoadedAsset->GetClass() ? StillLoadedAsset->GetClass()->GetName() : FString();
				if (LoadedClassName.Contains(TEXT("PCGGraph")))
				{
					UE_LOG(LogSOMOLMCPEditorServices, Warning,
						TEXT("SOMOLMCP: Disposable PCG graph is still loaded after pre-close/pre-GC; continuing DeleteAsset for smoke-owned temporary asset: %s"),
						*AssetPath);
				}
			}
		}

		if (!Subsystem->DeleteAsset(AssetPath))
		{
			const FString PrimaryDeleteError = FString::Printf(TEXT("Failed to delete asset: %s"), *AssetPath);
			if (!bDisposableAsset)
			{
				OutError = PrimaryDeleteError;
				return false;
			}

			if (!AssetExists(AssetPath))
			{
				return true;
			}

			if (UObject* DisposableObject = ResolveAssetObjectWithoutServiceLock(true))
			{
				TArray<UObject*> ObjectsToDelete;
				ObjectsToDelete.Add(DisposableObject);
				const int32 DeletedCount = ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
				if (DeletedCount > 0 && !AssetExists(AssetPath))
				{
					UE_LOG(LogSOMOLMCPEditorServices, Log,
						TEXT("SOMOLMCP: Deleted disposable asset with ObjectTools fallback after EditorAssetSubsystem failure: %s"),
						*AssetPath);
					return true;
				}
			}

			OutError = FString::Printf(TEXT("%s; disposable pre-close/pre-GC fallback still_exists=%s."),
				*PrimaryDeleteError,
				AssetExists(AssetPath) ? TEXT("true") : TEXT("false"));
			return false;
		}
		return true;
	}

	UObject* FSololmcpEditorServices::DuplicateAsset(const FString& SourceAssetPath, const FString& DestinationAssetPath, FString& OutError) const
	{
		UEditorAssetSubsystem* Subsystem = GetAssetSubsystem(OutError);
		if (!Subsystem)
		{
			return nullptr;
		}

		if (UObject* DuplicatedAsset = Subsystem->DuplicateAsset(SourceAssetPath, DestinationAssetPath))
		{
			return DuplicatedAsset;
		}

		OutError = FString::Printf(TEXT("Failed to duplicate asset from %s to %s"), *SourceAssetPath, *DestinationAssetPath);
		return nullptr;
	}

	bool FSololmcpEditorServices::RenameAsset(const FString& SourceAssetPath, const FString& DestinationAssetPath, FString& OutError) const
	{
		UEditorAssetSubsystem* Subsystem = GetAssetSubsystem(OutError);
		if (!Subsystem)
		{
			return false;
		}

		if (!Subsystem->RenameAsset(SourceAssetPath, DestinationAssetPath))
		{
			OutError = FString::Printf(TEXT("Failed to rename asset from %s to %s"), *SourceAssetPath, *DestinationAssetPath);
			return false;
		}
		return true;
	}

	bool FSololmcpEditorServices::MakeDirectory(const FString& DirectoryPath, FString& OutError) const
	{
		UEditorAssetSubsystem* Subsystem = GetAssetSubsystem(OutError);
		if (!Subsystem)
		{
			return false;
		}

		if (!Subsystem->MakeDirectory(DirectoryPath))
		{
			OutError = FString::Printf(TEXT("Failed to create directory: %s"), *DirectoryPath);
			return false;
		}
		return true;
	}

	TArray<FString> FSololmcpEditorServices::ListAssets(const FString& DirectoryPath, bool bRecursive, bool bIncludeFolders, FString& OutError) const
	{
		UEditorAssetSubsystem* Subsystem = GetAssetSubsystem(OutError);
		if (!Subsystem)
		{
			return {};
		}

		return Subsystem->ListAssets(DirectoryPath, bRecursive, bIncludeFolders);
	}

	TArray<FAssetData> FSololmcpEditorServices::QueryAssets(const FString& PackagePath, const FString& ClassPath, bool bRecursive, FString& OutError) const
	{
		FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& Registry = RegistryModule.Get();
		Registry.WaitForCompletion();

		FARFilter Filter;
		Filter.bRecursivePaths = bRecursive;
		Filter.bRecursiveClasses = true;

		if (!PackagePath.IsEmpty())
		{
			Filter.PackagePaths.Add(*PackagePath);
		}

		if (!ClassPath.IsEmpty())
		{
			FString ClassError;
			UClass* AssetClass = ResolveClass(ClassPath, ClassError);
			if (!AssetClass)
			{
				OutError = ClassError;
				return {};
			}
			Filter.ClassPaths.Add(AssetClass->GetClassPathName());
		}

		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		return Assets;
	}

	TArray<FString> FSololmcpEditorServices::GetAssetDependencies(const FString& AssetPath, FString& OutError) const
	{
		FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& Registry = RegistryModule.Get();
		Registry.WaitForCompletion();

		const FName PackageName(*FPackageName::ObjectPathToPackageName(AssetPath));
		TArray<FName> Dependencies;
		if (!Registry.GetDependencies(PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::All))
		{
			OutError = FString::Printf(TEXT("Failed to collect dependencies for %s"), *AssetPath);
			return {};
		}

		TArray<FString> OutDependencies;
		for (const FName& Dependency : Dependencies)
		{
			OutDependencies.Add(Dependency.ToString());
		}
		return OutDependencies;
	}

	bool FSololmcpEditorServices::AssetExists(const FString& AssetPath) const
	{
		// Round 12G: STRICT existence check.
		// Old fast-path used FindObject<UObject>(nullptr, *AssetPath) which matched empty
		// stub UPackages left behind by failed LoadAsset attempts (a UPackage IS a UObject).
		// That caused GenerateUniqueAssetName to falsely think the asset existed and bump
		// to "_01", and broke every downstream caller relying on the requested path.
		// Now: only return true if (a) the AssetRegistry actually has an entry for the
		// package, OR (b) FindObject finds a non-UPackage object inside the package.
		if (UObject* Found = FindObject<UObject>(nullptr, *AssetPath))
		{
			// Stub packages are bare UPackages with no inner asset. Walk the inners and
			// look for at least one real asset (non-UPackage, non-MetaData, IsAsset).
			if (UPackage* Pkg = Cast<UPackage>(Found))
			{
				bool bHasRealAsset = false;
				ForEachObjectWithOuter(Pkg, [&bHasRealAsset](UObject* Inner)
				{
					if (bHasRealAsset) { return; }
					if (Inner && !Inner->IsA<UPackage>() && Inner->IsAsset())
					{
						bHasRealAsset = true;
					}
				}, /*bIncludeNestedObjects*/ false);
				if (bHasRealAsset)
				{
					return true;
				}
				// fall through to AssetRegistry check
			}
			else
			{
				// Found is the asset itself (object path passed in) — definitely exists.
				return true;
			}
		}

		// Use AssetRegistry for a lightweight existence check (no load).
		FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& Registry = RegistryModule.Get();

		const FString PackageNameStr = FPackageName::ObjectPathToPackageName(AssetPath);
		if (PackageNameStr.IsEmpty())
		{
			return false;
		}

		const FName PackageFName(*PackageNameStr);
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(PackageFName, Assets);
		return Assets.Num() > 0;
	}

	FString FSololmcpEditorServices::GenerateUniqueAssetName(const FString& PackagePath, const FString& BaseName) const
	{
		// If the base name doesn't exist yet, return it as-is.
		const FString TestPath = PackagePath / BaseName;
		if (!AssetExists(TestPath))
		{
			return BaseName;
		}

		// Append _01, _02, ... until we find an unused name.
		for (int32 Suffix = 1; Suffix <= 999; ++Suffix)
		{
			const FString CandidateName = FString::Printf(TEXT("%s_%02d"), *BaseName, Suffix);
			const FString CandidatePath = PackagePath / CandidateName;
			if (!AssetExists(CandidatePath))
			{
				UE_LOG(LogSOMOLMCPEditorServices, Log, TEXT("SOMOLMCP: Auto-naming: '%s' already exists, using '%s' instead"), *BaseName, *CandidateName);
				return CandidateName;
			}
		}

		// Fallback — return BaseName_999 (extremely unlikely to collide).
		return FString::Printf(TEXT("%s_999"), *BaseName);
	}

	namespace
	{
		// Resolve a possibly nested property path ("ComponentName.PropertyName",
		// or deeper "Component.SubObject.Property") against a root object.
		// Intermediate segments resolve to actor components (when the current
		// owner is an AActor), object-valued UPROPERTYs, or named subobjects.
		bool ResolvePropertyPathTarget(UObject* RootObject, const FString& Path, UObject*& OutOwner, FProperty*& OutProperty, FString& OutError)
		{
			OutOwner = RootObject;
			OutProperty = nullptr;
			if (!RootObject || Path.IsEmpty())
			{
				OutError = TEXT("Invalid property path resolution request.");
				return false;
			}

			TArray<FString> Segments;
			Path.ParseIntoArray(Segments, TEXT("."), /*bCullEmpty=*/true);
			if (Segments.Num() == 0)
			{
				OutError = FString::Printf(TEXT("Property path '%s' is empty."), *Path);
				return false;
			}

			for (int32 Index = 0; Index < Segments.Num() - 1; ++Index)
			{
				const FString& Segment = Segments[Index];
				UObject* NextOwner = nullptr;
				if (AActor* ActorOwner = Cast<AActor>(OutOwner))
				{
					TInlineComponentArray<UActorComponent*> Components;
					ActorOwner->GetComponents(Components);
					for (UActorComponent* Component : Components)
					{
						if (Component && Component->GetName().Equals(Segment, ESearchCase::IgnoreCase))
						{
							NextOwner = Component;
							break;
						}
					}
				}
				if (!NextOwner)
				{
					if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(OutOwner->GetClass()->FindPropertyByName(*Segment)))
					{
						NextOwner = ObjectProperty->GetObjectPropertyValue(ObjectProperty->ContainerPtrToValuePtr<void>(OutOwner));
					}
				}
				if (!NextOwner)
				{
					NextOwner = FindObject<UObject>(OutOwner, *Segment);
				}
				if (!NextOwner)
				{
					OutError = FString::Printf(TEXT("Property path segment '%s' did not resolve to a component or sub-object on %s."), *Segment, *OutOwner->GetClass()->GetName());
					return false;
				}
				OutOwner = NextOwner;
			}

			const FString& LeafName = Segments.Last();
			OutProperty = OutOwner->GetClass()->FindPropertyByName(*LeafName);
			if (!OutProperty)
			{
				OutError = FString::Printf(TEXT("Property '%s' was not found on %s"), *LeafName, *OutOwner->GetClass()->GetName());
				return false;
			}
			return true;
		}

		// Export a reflected property value as JSON for receipt readback.
		TSharedPtr<FJsonValue> PropertyValueToJson(const void* ValuePtr, const FProperty* Property)
		{
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				const UObject* Value = ObjectProperty->GetObjectPropertyValue(ValuePtr);
				return MakeShared<FJsonValueString>(Value ? Value->GetPathName() : TEXT("None"));
			}
			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				if (const FObjectPropertyBase* InnerObjectProperty = CastField<FObjectPropertyBase>(ArrayProperty->Inner))
				{
					TArray<TSharedPtr<FJsonValue>> Items;
					FScriptArrayHelper ArrayHelper(ArrayProperty, const_cast<void*>(ValuePtr));
					for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
					{
						const UObject* Element = InnerObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index));
						Items.Add(MakeShared<FJsonValueString>(Element ? Element->GetPathName() : TEXT("None")));
					}
					return MakeShared<FJsonValueArray>(Items);
				}
			}
			FString Exported;
			Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
			return MakeShared<FJsonValueString>(Exported);
		}
	}

	bool FSololmcpEditorServices::ApplyProperties(UObject* TargetObject, const TSharedPtr<FJsonObject>& Properties, FString& OutError) const
	{
		TArray<TSharedPtr<FJsonValue>> DiscardedReceipts;
		return ApplyPropertiesWithReceipts(TargetObject, Properties, DiscardedReceipts, OutError);
	}

	bool FSololmcpEditorServices::ApplyPropertiesWithReceipts(UObject* TargetObject, const TSharedPtr<FJsonObject>& Properties, TArray<TSharedPtr<FJsonValue>>& OutReceipts, FString& OutError) const
	{
		if (!TargetObject || !Properties.IsValid())
		{
			return true;
		}

		TSet<UObject*> TouchedOwners;
		for (const auto& Pair : Properties->Values)
		{
			const FString Key(*Pair.Key);
			UObject* Owner = TargetObject;
			FProperty* Property = nullptr;
			if (!ResolvePropertyPathTarget(TargetObject, Key, Owner, Property, OutError))
			{
				return false;
			}

			Owner->Modify();
			if (!ApplyJsonValueToProperty(Property->ContainerPtrToValuePtr<void>(Owner), Property, Pair.Value, OutError))
			{
				return false;
			}
			TouchedOwners.Add(Owner);

			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			Receipt->SetStringField(TEXT("property"), Key);
			Receipt->SetStringField(TEXT("target_object"), Owner->GetName());
			Receipt->SetStringField(TEXT("target_class"), Owner->GetClass()->GetName());
			Receipt->SetBoolField(TEXT("applied"), true);
			Receipt->SetField(TEXT("readback"), PropertyValueToJson(Property->ContainerPtrToValuePtr<void>(Owner), Property));
			OutReceipts.Add(MakeShared<FJsonValueObject>(Receipt));
		}

		for (UObject* Owner : TouchedOwners)
		{
			Owner->PostEditChange();
		}
		return true;
	}

	TSharedRef<FJsonObject> FSololmcpEditorServices::MakeObjectReference(const UObject* Object)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Object)
		{
			Json->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
			return Json;
		}

		Json->SetStringField(TEXT("name"), Object->GetName());
		Json->SetStringField(TEXT("path"), Object->GetPathName());
		Json->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
		return Json;
	}

	TSharedRef<FJsonObject> FSololmcpEditorServices::MakeActorReference(const AActor* Actor)
	{
		TSharedRef<FJsonObject> Json = MakeObjectReference(Actor);
		if (Actor)
		{
			Json->SetStringField(TEXT("label"), Actor->GetActorLabel());
		}
		return Json;
	}

	bool FSololmcpEditorServices::JsonToVector(const TSharedPtr<FJsonObject>& Object, FVector& OutVector)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!Object->TryGetNumberField(TEXT("x"), X) || !Object->TryGetNumberField(TEXT("y"), Y) || !Object->TryGetNumberField(TEXT("z"), Z))
		{
			return false;
		}

		OutVector = FVector(X, Y, Z);
		return true;
	}

	bool FSololmcpEditorServices::JsonToRotator(const TSharedPtr<FJsonObject>& Object, FRotator& OutRotator)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		double Pitch = 0.0;
		double Yaw = 0.0;
		double Roll = 0.0;
		if (!Object->TryGetNumberField(TEXT("pitch"), Pitch) || !Object->TryGetNumberField(TEXT("yaw"), Yaw) || !Object->TryGetNumberField(TEXT("roll"), Roll))
		{
			return false;
		}

		OutRotator = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	bool FSololmcpEditorServices::JsonToLinearColor(const TSharedPtr<FJsonObject>& Object, FLinearColor& OutColor)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 1.0;
		// Accept both lowercase and uppercase field names
		if (!(Object->TryGetNumberField(TEXT("r"), R) || Object->TryGetNumberField(TEXT("R"), R)) ||
			!(Object->TryGetNumberField(TEXT("g"), G) || Object->TryGetNumberField(TEXT("G"), G)) ||
			!(Object->TryGetNumberField(TEXT("b"), B) || Object->TryGetNumberField(TEXT("B"), B)))
		{
			return false;
		}
		Object->TryGetNumberField(TEXT("a"), A);
		if (A == 1.0) { Object->TryGetNumberField(TEXT("A"), A); }

		OutColor = FLinearColor(R, G, B, A);
		return true;
	}

	bool FSololmcpEditorServices::JsonToTransform(const TSharedPtr<FJsonObject>& Object, FTransform& OutTransform)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;

		if (const TSharedPtr<FJsonObject>* LocationObject = nullptr; Object->TryGetObjectField(TEXT("location"), LocationObject) && LocationObject)
		{
			JsonToVector(*LocationObject, Location);
		}
		if (const TSharedPtr<FJsonObject>* RotationObject = nullptr; Object->TryGetObjectField(TEXT("rotation"), RotationObject) && RotationObject)
		{
			JsonToRotator(*RotationObject, Rotation);
		}
		if (const TSharedPtr<FJsonObject>* ScaleObject = nullptr; Object->TryGetObjectField(TEXT("scale"), ScaleObject) && ScaleObject)
		{
			JsonToVector(*ScaleObject, Scale);
		}

		OutTransform = FTransform(Rotation, Location, Scale);
		return true;
	}

	UFactory* FSololmcpEditorServices::CreateFactory(const FString& FactoryClassPath, const TSharedPtr<FJsonObject>& FactoryOverrides, FString& OutError) const
	{
		if (FactoryClassPath.IsEmpty())
		{
			OutError = TEXT("Factory class path is empty.");
			return nullptr;
		}

		UClass* FactoryClass = ResolveClass(FactoryClassPath, OutError);
		if (!FactoryClass)
		{
			return nullptr;
		}

		UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
		if (!Factory)
		{
			OutError = FString::Printf(TEXT("Failed to instantiate factory: %s"), *FactoryClassPath);
			return nullptr;
		}

		if (FactoryOverrides.IsValid() && !ApplyProperties(Factory, FactoryOverrides, OutError))
		{
			return nullptr;
		}

		return Factory;
	}

	bool FSololmcpEditorServices::ApplyJsonValueToProperty(void* ValuePtr, FProperty* Property, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError) const
	{
		if (!ValuePtr || !Property || !JsonValue.IsValid())
		{
			OutError = TEXT("Invalid reflected-property JSON assignment target or value.");
			return false;
		}

		const bool bNull = JsonValue->Type == EJson::Null;
		auto RequireType = [&OutError, Property](const bool bCondition, const TCHAR* Expected) -> bool
		{
			if (!bCondition)
			{
				OutError = FString::Printf(TEXT("Property '%s' (%s) expects %s."),
					*Property->GetName(), *Property->GetCPPType(), Expected);
				return false;
			}
			return true;
		};

		// Soft references must be handled before FObjectPropertyBase: in UE they
		// derive from the object-property hierarchy but their storage is an
		// FSoftObjectPtr, not a raw UObject pointer.
		if (FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
		{
			FString ClassPath;
			if (!bNull && !JsonValue->TryGetString(ClassPath))
			{
				return RequireType(false, TEXT("a soft class path string or null"));
			}
			const FString ImportValue = bNull || ClassPath.IsEmpty() ? TEXT("None") : ClassPath;
			if (!SoftClassProperty->ImportText_Direct(*ImportValue, ValuePtr, nullptr, PPF_None))
			{
				OutError = FString::Printf(TEXT("Soft class path '%s' could not be imported into '%s'."), *ClassPath, *Property->GetName());
				return false;
			}
			return true;
		}

		if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			FString AssetPath;
			if (!bNull && !JsonValue->TryGetString(AssetPath))
			{
				return RequireType(false, TEXT("a soft object path string or null"));
			}
			const FString ImportValue = bNull || AssetPath.IsEmpty() ? TEXT("None") : AssetPath;
			if (!SoftObjectProperty->ImportText_Direct(*ImportValue, ValuePtr, nullptr, PPF_None))
			{
				OutError = FString::Printf(TEXT("Soft object path '%s' could not be imported into '%s'."), *AssetPath, *Property->GetName());
				return false;
			}
			return true;
		}

		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
			if (!JsonValue->TryGetArray(JsonArray) || !JsonArray)
			{
				return RequireType(false, TEXT("a JSON array"));
			}

			FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
			ArrayHelper.Resize(JsonArray->Num());
			for (int32 Index = 0; Index < JsonArray->Num(); ++Index)
			{
				FString ElementError;
				if (!ApplyJsonValueToProperty(ArrayHelper.GetRawPtr(Index), ArrayProperty->Inner, (*JsonArray)[Index], ElementError))
				{
					OutError = FString::Printf(TEXT("%s[%d]: %s"), *Property->GetName(), Index, *ElementError);
					return false;
				}
			}
			return true;
		}

		if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
			if (!JsonValue->TryGetArray(JsonArray) || !JsonArray)
			{
				return RequireType(false, TEXT("a JSON array (set elements must be unique after conversion)"));
			}

			FScriptSetHelper SetHelper(SetProperty, ValuePtr);
			SetHelper.EmptyElements(JsonArray->Num());
			for (int32 Index = 0; Index < JsonArray->Num(); ++Index)
			{
				const int32 InternalIndex = SetHelper.AddDefaultValue_Invalid_NeedsRehash();
				FString ElementError;
				if (!ApplyJsonValueToProperty(SetHelper.GetElementPtr(InternalIndex), SetProperty->ElementProp, (*JsonArray)[Index], ElementError))
				{
					OutError = FString::Printf(TEXT("%s[%d]: %s"), *Property->GetName(), Index, *ElementError);
					return false;
				}
			}
			SetHelper.Rehash();
			return true;
		}

		if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			TArray<TPair<TSharedPtr<FJsonValue>, TSharedPtr<FJsonValue>>> Entries;
			if (JsonValue->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject>* JsonObject = nullptr;
				JsonValue->TryGetObject(JsonObject);
				if (!JsonObject || !JsonObject->IsValid())
				{
					return RequireType(false, TEXT("a JSON object or [{\"key\":...,\"value\":...}] array"));
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*JsonObject)->Values)
				{
					Entries.Emplace(MakeShared<FJsonValueString>(Pair.Key), Pair.Value);
				}
			}
			else if (JsonValue->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& JsonEntries = JsonValue->AsArray();
				for (int32 Index = 0; Index < JsonEntries.Num(); ++Index)
				{
					const TSharedPtr<FJsonObject>* EntryObject = nullptr;
					if (!JsonEntries[Index].IsValid() || !JsonEntries[Index]->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
					{
						OutError = FString::Printf(TEXT("%s[%d] expects an object containing 'key' and 'value'."), *Property->GetName(), Index);
						return false;
					}
					const TSharedPtr<FJsonValue> Key = (*EntryObject)->TryGetField(TEXT("key"));
					const TSharedPtr<FJsonValue> Value = (*EntryObject)->TryGetField(TEXT("value"));
					if (!Key.IsValid() || !Value.IsValid())
					{
						OutError = FString::Printf(TEXT("%s[%d] is missing 'key' or 'value'."), *Property->GetName(), Index);
						return false;
					}
					Entries.Emplace(Key, Value);
				}
			}
			else
			{
				return RequireType(false, TEXT("a JSON object or [{\"key\":...,\"value\":...}] array"));
			}

			FScriptMapHelper MapHelper(MapProperty, ValuePtr);
			MapHelper.EmptyValues(Entries.Num());
			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				const int32 InternalIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
				FString EntryError;
				if (!ApplyJsonValueToProperty(MapHelper.GetKeyPtr(InternalIndex), MapProperty->KeyProp, Entries[Index].Key, EntryError))
				{
					OutError = FString::Printf(TEXT("%s[%d].key: %s"), *Property->GetName(), Index, *EntryError);
					return false;
				}
				if (!ApplyJsonValueToProperty(MapHelper.GetValuePtr(InternalIndex), MapProperty->ValueProp, Entries[Index].Value, EntryError))
				{
					OutError = FString::Printf(TEXT("%s[%d].value: %s"), *Property->GetName(), Index, *EntryError);
					return false;
				}
			}
			MapHelper.Rehash();
			return true;
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const TSharedPtr<FJsonObject>* StructJson = nullptr;
			if (!JsonValue->TryGetObject(StructJson) || !StructJson || !StructJson->IsValid())
			{
				return RequireType(false, TEXT("a JSON object"));
			}
			if (!ApplyJsonObjectToStruct(ValuePtr, StructProperty->Struct, *StructJson, OutError))
			{
				OutError = FString::Printf(TEXT("%s: %s"), *Property->GetName(), *OutError);
				return false;
			}
			return true;
		}

		if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			FString Value;
			if (!JsonValue->TryGetString(Value))
			{
				return RequireType(false, TEXT("a string"));
			}
			StrProperty->SetPropertyValue(ValuePtr, Value);
			return true;
		}

		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			FString Value;
			if (!JsonValue->TryGetString(Value))
			{
				return RequireType(false, TEXT("a name string"));
			}
			NameProperty->SetPropertyValue(ValuePtr, *Value);
			return true;
		}

		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			FString Value;
			if (!JsonValue->TryGetString(Value))
			{
				return RequireType(false, TEXT("a text string"));
			}
			TextProperty->SetPropertyValue(ValuePtr, FText::FromString(Value));
			return true;
		}

		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			bool bValue = false;
			if (!JsonValue->TryGetBool(bValue))
			{
				return RequireType(false, TEXT("a boolean"));
			}
			BoolProperty->SetPropertyValue(ValuePtr, bValue);
			return true;
		}

		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property);
			NumericProperty && !CastField<FByteProperty>(Property))
		{
			double Number = 0.0;
			if (!JsonValue->TryGetNumber(Number) || !FMath::IsFinite(Number))
			{
				return RequireType(false, NumericProperty->IsInteger() ? TEXT("a finite integer") : TEXT("a finite number"));
			}
			if (NumericProperty->IsInteger())
			{
				if (!FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
				{
					return RequireType(false, TEXT("an integer without a fractional part"));
				}
				const int64 IntegerValue = static_cast<int64>(Number);
				if (!NumericProperty->CanHoldValue(IntegerValue))
				{
					OutError = FString::Printf(TEXT("Integer value %.0f is outside the range of property '%s' (%s)."),
						Number, *Property->GetName(), *Property->GetCPPType());
					return false;
				}
				NumericProperty->SetIntPropertyValue(ValuePtr, IntegerValue);
			}
			else
			{
				NumericProperty->SetFloatingPointPropertyValue(ValuePtr, Number);
			}
			return true;
		}

		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			FString EnumName;
			if (JsonValue->TryGetString(EnumName))
			{
				const int64 EnumValue = EnumProperty->GetEnum()->GetValueByNameString(EnumName);
				if (EnumValue == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("Invalid enum value '%s' for property '%s'."), *EnumName, *Property->GetName());
					return false;
				}
				EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumValue);
				return true;
			}

			double NumericEnum = 0.0;
			if (!JsonValue->TryGetNumber(NumericEnum) || !FMath::IsNearlyEqual(NumericEnum, FMath::RoundToDouble(NumericEnum)))
			{
				return RequireType(false, TEXT("an enum name string or integer value"));
			}
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, static_cast<int64>(NumericEnum));
			return true;
		}

		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			// UE still represents a number of reflected enums (for example
			// EAutoReceiveInput) as FByteProperty + UEnum rather than FEnumProperty.
			// Treating a JSON string as AsNumber() silently produced zero/Disabled
			// while the receipt incorrectly reported applied=true.  Resolve both
			// canonical enum names and localized display names, and fail closed when
			// the caller supplies an unknown label.
			if (ByteProperty->Enum)
			{
				FString EnumName;
				if (JsonValue->TryGetString(EnumName))
				{
					int64 EnumValue = ByteProperty->Enum->GetValueByNameString(EnumName);
					if (EnumValue == INDEX_NONE)
					{
						for (int32 EnumIndex = 0; EnumIndex < ByteProperty->Enum->NumEnums(); ++EnumIndex)
						{
							if (ByteProperty->Enum->HasMetaData(TEXT("Hidden"), EnumIndex)) continue;
							if (ByteProperty->Enum->GetDisplayNameTextByIndex(EnumIndex).ToString().Equals(
								EnumName, ESearchCase::IgnoreCase))
							{
								EnumValue = ByteProperty->Enum->GetValueByIndex(EnumIndex);
								break;
							}
						}
					}
					if (EnumValue == INDEX_NONE || EnumValue < 0 || EnumValue > MAX_uint8)
					{
						OutError = FString::Printf(TEXT("Invalid enum value '%s' for byte-enum property '%s'."),
							*EnumName, *Property->GetName());
						return false;
					}
					ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumValue));
					return true;
				}
			}
			double ByteNumber = 0.0;
			if (!JsonValue->TryGetNumber(ByteNumber) || ByteNumber < 0.0 || ByteNumber > 255.0 || !FMath::IsNearlyEqual(ByteNumber, FMath::RoundToDouble(ByteNumber)))
			{
				return RequireType(false, ByteProperty->Enum ? TEXT("an enum name string or integer 0..255") : TEXT("an integer 0..255"));
			}
			ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(ByteNumber));
			return true;
		}

		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			FString AssetPath;
			if (!bNull && !JsonValue->TryGetString(AssetPath))
			{
				return RequireType(false, TEXT("an object/class path string or null"));
			}

			const bool bNone = bNull || AssetPath.IsEmpty() || AssetPath.Equals(TEXT("None"), ESearchCase::IgnoreCase);
			UObject* ResolvedObject = bNone ? nullptr : LoadObject<UObject>(nullptr, *AssetPath);
			if (!bNone && !ResolvedObject)
			{
				OutError = FString::Printf(TEXT("Object '%s' could not be loaded for property '%s'."), *AssetPath, *Property->GetName());
				return false;
			}
			if (ResolvedObject && !ResolvedObject->IsA(ObjectProperty->PropertyClass))
			{
				OutError = FString::Printf(TEXT("Object '%s' is not of type '%s'."), *AssetPath, *ObjectProperty->PropertyClass->GetName());
				return false;
			}

			ObjectProperty->SetObjectPropertyValue(ValuePtr, ResolvedObject);
			return true;
		}

		OutError = FString::Printf(TEXT("Property '%s' has unsupported reflected type '%s'."), *Property->GetName(), *Property->GetCPPType());
		return false;
	}

	bool FSololmcpEditorServices::ApplyJsonObjectToStruct(void* StructPtr, UScriptStruct* StructType, const TSharedPtr<FJsonObject>& JsonObject, FString& OutError) const
	{
		if (!StructType || !JsonObject.IsValid())
		{
			OutError = TEXT("Invalid struct assignment request.");
			return false;
		}

		if (StructType == TBaseStructure<FVector>::Get())
		{
			FVector VectorValue = FVector::ZeroVector;
			if (!JsonToVector(JsonObject, VectorValue))
			{
				OutError = TEXT("Failed to parse FVector.");
				return false;
			}
			*static_cast<FVector*>(StructPtr) = VectorValue;
			return true;
		}

		if (StructType == TBaseStructure<FRotator>::Get())
		{
			FRotator RotatorValue = FRotator::ZeroRotator;
			if (!JsonToRotator(JsonObject, RotatorValue))
			{
				OutError = TEXT("Failed to parse FRotator.");
				return false;
			}
			*static_cast<FRotator*>(StructPtr) = RotatorValue;
			return true;
		}

		if (StructType == TBaseStructure<FLinearColor>::Get())
		{
			FLinearColor ColorValue = FLinearColor::White;
			if (!JsonToLinearColor(JsonObject, ColorValue))
			{
				OutError = TEXT("Failed to parse FLinearColor.");
				return false;
			}
			*static_cast<FLinearColor*>(StructPtr) = ColorValue;
			return true;
		}

		if (StructType == TBaseStructure<FTransform>::Get())
		{
			FTransform TransformValue = FTransform::Identity;
			if (!JsonToTransform(JsonObject, TransformValue))
			{
				OutError = TEXT("Failed to parse FTransform.");
				return false;
			}
			*static_cast<FTransform*>(StructPtr) = TransformValue;
			return true;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : JsonObject->Values)
		{
			FProperty* Property = StructType->FindPropertyByName(FName(*Pair.Key));
			if (!Property)
			{
				for (TFieldIterator<FProperty> It(StructType); It; ++It)
				{
					if (It->GetName().Equals(Pair.Key, ESearchCase::IgnoreCase))
					{
						Property = *It;
						break;
					}
				}
			}
			if (!Property)
			{
				OutError = FString::Printf(TEXT("Unknown field '%s' for struct '%s'."), *Pair.Key, *StructType->GetName());
				return false;
			}

			if (!ApplyJsonValueToProperty(Property->ContainerPtrToValuePtr<void>(StructPtr), Property, Pair.Value, OutError))
			{
				OutError = FString::Printf(TEXT("%s.%s: %s"), *StructType->GetName(), *Pair.Key, *OutError);
				return false;
			}
		}
		return true;
	}

	// ── Screenshot Capture Implementation ──────────────────────────────────

	bool FSololmcpEditorServices::CompressPixelsToPng(const TArray<FColor>& Pixels, int32 Width, int32 Height, TArray<uint8>& OutPngData, FString& OutError)
	{
		if (Pixels.Num() == 0 || Width <= 0 || Height <= 0)
		{
			OutError = TEXT("Empty pixel data.");
			return false;
		}

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!ImageWrapper.IsValid())
		{
			OutError = TEXT("Failed to create PNG image wrapper.");
			return false;
		}

		if (!ImageWrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			OutError = TEXT("Failed to set raw pixel data on image wrapper.");
			return false;
		}

		const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed(100);
		if (CompressedData.Num() == 0)
		{
			OutError = TEXT("PNG compression produced empty output.");
			return false;
		}

		OutPngData.Reset(CompressedData.Num());
		OutPngData.Append(CompressedData.GetData(), CompressedData.Num());
		return true;
	}

	bool FSololmcpEditorServices::CaptureViewportScreenshot(
		TArray<uint8>& OutPngData,
		int32 MaxWidth,
		int32 MaxHeight,
		FString& OutError,
		const bool bExactResolution) const
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is not available.");
			return false;
		}

		FViewport* Viewport = GEditor->GetActiveViewport();
		if (!Viewport)
		{
			OutError = TEXT("No active viewport found.");
			return false;
		}

		const FIntPoint ViewportSize = Viewport->GetSizeXY();
		if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
		{
			OutError = TEXT("Viewport has zero dimensions.");
			return false;
		}

		// FIX (v11): in MCP remote-driven mode the editor window doesn't get
		// focus events, so its back buffer is whatever was rendered the last
		// time a human interacted with the viewport. ReadPixels() then returns
		// that stale frame for every screenshot, regardless of scene changes.
		// Force a fresh redraw before reading.
		//
		// Sequence:
		//   1. RedrawAllViewports(true)  -> queues a redraw of every editor
		//      viewport, true forces immediate flush instead of next-tick.
		//   2. Viewport->Draw()          -> belt-and-suspenders synchronous
		//      draw on the specific viewport we're about to capture.
		//   3. FlushRenderingCommands()  -> blocks until the GPU finishes the
		//      queued draw, so back buffer holds the new frame before ReadPixels.
		if (GEditor)
		{
			GEditor->RedrawAllViewports(/*bInvalidateHitProxies=*/true);
		}
		Viewport->InvalidateHitProxy();
		Viewport->Draw();
		FlushRenderingCommands();

		TArray<FColor> Bitmap;
		if (!Viewport->ReadPixels(Bitmap))
		{
			OutError = TEXT("Failed to read viewport pixels.");
			return false;
		}

		int32 FinalWidth = ViewportSize.X;
		int32 FinalHeight = ViewportSize.Y;

		if (bExactResolution && (FinalWidth != MaxWidth || FinalHeight != MaxHeight))
		{
			TArray<FColor> Resized;
			Resized.SetNumUninitialized(MaxWidth * MaxHeight);
			FImageUtils::ImageResize(FinalWidth, FinalHeight, Bitmap, MaxWidth, MaxHeight, Resized, true);

			Bitmap = MoveTemp(Resized);
			FinalWidth = MaxWidth;
			FinalHeight = MaxHeight;
		}
		else if (!bExactResolution && (FinalWidth > MaxWidth || FinalHeight > MaxHeight))
		{
			const float ScaleX = static_cast<float>(MaxWidth) / static_cast<float>(FinalWidth);
			const float ScaleY = static_cast<float>(MaxHeight) / static_cast<float>(FinalHeight);
			const float Scale = FMath::Min(ScaleX, ScaleY);
			const int32 NewWidth = FMath::Max(1, FMath::RoundToInt(FinalWidth * Scale));
			const int32 NewHeight = FMath::Max(1, FMath::RoundToInt(FinalHeight * Scale));

			TArray<FColor> Resized;
			Resized.SetNumUninitialized(NewWidth * NewHeight);
			FImageUtils::ImageResize(FinalWidth, FinalHeight, Bitmap, NewWidth, NewHeight, Resized, true);

			Bitmap = MoveTemp(Resized);
			FinalWidth = NewWidth;
			FinalHeight = NewHeight;
		}

		return CompressPixelsToPng(Bitmap, FinalWidth, FinalHeight, OutPngData, OutError);
	}

	bool FSololmcpEditorServices::CaptureSlateWidgetScreenshot(TSharedPtr<SWidget> Widget, TArray<uint8>& OutPngData, int32 MaxWidth, int32 MaxHeight, FString& OutError) const
	{
		if (!Widget.IsValid())
		{
			OutError = TEXT("Invalid widget pointer.");
			return false;
		}

		const FVector2D DesiredSize = Widget->GetDesiredSize();
		int32 CaptureWidth = FMath::Max(1, FMath::RoundToInt(DesiredSize.X));
		int32 CaptureHeight = FMath::Max(1, FMath::RoundToInt(DesiredSize.Y));

		if (CaptureWidth <= 1 || CaptureHeight <= 1)
		{
			const FGeometry& Geometry = Widget->GetCachedGeometry();
			CaptureWidth = FMath::Max(1, FMath::RoundToInt(Geometry.GetAbsoluteSize().X));
			CaptureHeight = FMath::Max(1, FMath::RoundToInt(Geometry.GetAbsoluteSize().Y));
		}

		if (CaptureWidth <= 1 || CaptureHeight <= 1)
		{
			OutError = TEXT("Widget has zero or unknown dimensions. Ensure the editor tab is visible.");
			return false;
		}

		const float ScaleDownFactor = FMath::Min(
			1.0f,
			FMath::Min(
				static_cast<float>(MaxWidth) / static_cast<float>(CaptureWidth),
				static_cast<float>(MaxHeight) / static_cast<float>(CaptureHeight)
			)
		);

		const int32 RenderWidth = FMath::Max(1, FMath::RoundToInt(CaptureWidth * ScaleDownFactor));
		const int32 RenderHeight = FMath::Max(1, FMath::RoundToInt(CaptureHeight * ScaleDownFactor));

		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		RenderTarget->ClearColor = FLinearColor::Black;
		RenderTarget->InitAutoFormat(RenderWidth, RenderHeight);
		RenderTarget->UpdateResourceImmediate(true);

		TUniquePtr<FWidgetRenderer> WidgetRenderer(new FWidgetRenderer(true, false));
		WidgetRenderer->DrawWidget(RenderTarget, Widget.ToSharedRef(), ScaleDownFactor, FVector2D(CaptureWidth, CaptureHeight), 0.0f);

		FlushRenderingCommands();

		FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!Resource)
		{
			OutError = TEXT("Failed to get render target resource.");
			return false;
		}

		TArray<FColor> Pixels;
		if (!Resource->ReadPixels(Pixels))
		{
			OutError = TEXT("Failed to read pixels from render target.");
			return false;
		}

		return CompressPixelsToPng(Pixels, RenderWidth, RenderHeight, OutPngData, OutError);
	}
}
