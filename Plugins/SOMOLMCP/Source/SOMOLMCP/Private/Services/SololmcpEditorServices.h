// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "HAL/CriticalSection.h"

class AActor;
class UEditorActorSubsystem;
class UEditorAssetSubsystem;
class ULevelEditorSubsystem;
class UAssetEditorSubsystem;
class UFactory;
class UWorld;
class SWidget;

namespace UE::SOMOLMCP
{
	// Foliage concurrency guard (2026-08-05 audit): every foliage_* tool mutates
	// the shared AInstancedFoliageActor / FFoliageInfo. Concurrent worker dispatch
	// crashed the instance map (TMap corruption + leaked FRWLock), so all foliage
	// tool execution is serialized through this mutex at the dispatch layer
	// (JobService SEH guard, Router sync lane and data-driven macro steps).
	// Lock/Unlock are crash-safe: SEH __except handlers call UnlockFoliageActorMutex
	// so a crashed tool never leaks the lock, and the thread-local re-entrancy flag
	// prevents self-deadlock when a tool chains another foliage tool.
	FCriticalSection& GetFoliageActorMutex();
	/** True for any tool whose name starts with foliage_ (all touch the shared actor). */
	bool IsFoliageActorTool(const FString& ToolName);
	void LockFoliageActorMutex();
	void UnlockFoliageActorMutex();

	class FSololmcpEditorServices
	{
	public:
		bool IsPythonAvailable(FString* OutReason = nullptr) const;
		bool ExecutePython(const FString& Code, const FString& Mode, bool bUnattended, TSharedRef<class FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) const;
		bool ExecuteConsole(const FString& Command, TSharedRef<class FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) const;

		UWorld* GetEditorWorld(FString& OutError) const;

		/**
		 * Editor-Runtime Parity (ERP) world resolution: returns the active PIE/game
		 * world when one exists (runtime authority wins), otherwise the editor world.
		 * Single entry point for WorldSubsystem-backed tools so they behave identically
		 * in the editor and under PIE.
		 */
		UWorld* ResolveActiveWorld(FString& OutError) const;
		/** Returns only a live PIE/game world (GameInstanceSubsystem-backed paths). */
		UWorld* ResolveGameWorld(FString& OutError) const;
		/** Resolves a WorldSubsystem on the active world (PIE/game first, editor fallback). */
		template <typename T>
		T* ResolveWorldSubsystem(FString& OutError) const
		{
			if (UWorld* World = ResolveActiveWorld(OutError))
			{
				if (T* Subsystem = World->GetSubsystem<T>())
				{
					return Subsystem;
				}
				OutError = FString::Printf(TEXT("WorldSubsystem '%s' is not available in the active world."), *T::StaticClass()->GetName());
				return nullptr;
			}
			return nullptr;
		}

		UEditorActorSubsystem* GetActorSubsystem(FString& OutError) const;
		UEditorAssetSubsystem* GetAssetSubsystem(FString& OutError) const;
		ULevelEditorSubsystem* GetLevelEditorSubsystem(FString& OutError) const;
		UAssetEditorSubsystem* GetAssetEditorSubsystem(FString& OutError) const;

		UObject* LoadAsset(const FString& AssetPath, FString& OutError) const;
		UClass* ResolveClass(const FString& ClassPath, FString& OutError) const;
		AActor* FindActorByLabelOrName(const FString& ActorId, FString& OutError) const;

		UObject* CreateAsset(const FString& PackagePath, const FString& AssetName, const FString& AssetClassPath, const FString& FactoryClassPath, const TSharedPtr<FJsonObject>& FactoryOverrides, FString& OutError, bool bReplaceExisting = false) const;
		bool SaveAsset(const FString& AssetPath, bool bOnlyIfDirty, FString& OutError) const;
		bool DeleteAsset(const FString& AssetPath, FString& OutError) const;
		UObject* DuplicateAsset(const FString& SourceAssetPath, const FString& DestinationAssetPath, FString& OutError) const;
		bool RenameAsset(const FString& SourceAssetPath, const FString& DestinationAssetPath, FString& OutError) const;
		bool MakeDirectory(const FString& DirectoryPath, FString& OutError) const;
		TArray<FString> ListAssets(const FString& DirectoryPath, bool bRecursive, bool bIncludeFolders, FString& OutError) const;
		TArray<FAssetData> QueryAssets(const FString& PackagePath, const FString& ClassPath, bool bRecursive, FString& OutError) const;
		TArray<FString> GetAssetDependencies(const FString& AssetPath, FString& OutError) const;

		/** Generate a unique asset name by appending _01, _02, etc. suffix if the target already exists. */
		FString GenerateUniqueAssetName(const FString& PackagePath, const FString& BaseName) const;

		/** Check if an asset exists at the given path (e.g. "/Game/Folder/AssetName"). */
		bool AssetExists(const FString& AssetPath) const;

		bool ApplyProperties(UObject* TargetObject, const TSharedPtr<FJsonObject>& Properties, FString& OutError) const;
		/** Same as ApplyProperties, plus per-property receipts with post-set readback values. Supports "ComponentName.PropertyName" (and deeper object) paths on actors. */
		bool ApplyPropertiesWithReceipts(UObject* TargetObject, const TSharedPtr<FJsonObject>& Properties, TArray<TSharedPtr<FJsonValue>>& OutReceipts, FString& OutError) const;
		/** Recursively import typed JSON into initialized reflected-property storage. The caller owns validation/transaction boundaries. */
		bool ApplyJsonValueToProperty(void* ValuePtr, FProperty* Property, const TSharedPtr<class FJsonValue>& JsonValue, FString& OutError) const;

		// Screenshot capture
		// bExactResolution=false preserves the historical "maximum bounds" contract.
		// bExactResolution=true always returns exactly Width x Height pixels (including
		// deterministic upscaling when the active editor viewport is smaller).
		bool CaptureViewportScreenshot(
			TArray<uint8>& OutPngData,
			int32 Width,
			int32 Height,
			FString& OutError,
			bool bExactResolution = false) const;
		bool CaptureSlateWidgetScreenshot(TSharedPtr<SWidget> Widget, TArray<uint8>& OutPngData, int32 MaxWidth, int32 MaxHeight, FString& OutError) const;
		static bool CompressPixelsToPng(const TArray<FColor>& Pixels, int32 Width, int32 Height, TArray<uint8>& OutPngData, FString& OutError);

		static TSharedRef<class FJsonObject> MakeObjectReference(const UObject* Object);
		static TSharedRef<class FJsonObject> MakeActorReference(const AActor* Actor);

		static bool JsonToVector(const TSharedPtr<FJsonObject>& Object, FVector& OutVector);
		static bool JsonToRotator(const TSharedPtr<FJsonObject>& Object, FRotator& OutRotator);
		static bool JsonToLinearColor(const TSharedPtr<FJsonObject>& Object, FLinearColor& OutColor);
		static bool JsonToTransform(const TSharedPtr<FJsonObject>& Object, FTransform& OutTransform);

	private:
		UFactory* CreateFactory(const FString& FactoryClassPath, const TSharedPtr<FJsonObject>& FactoryOverrides, FString& OutError) const;
		bool ApplyJsonObjectToStruct(void* StructPtr, class UScriptStruct* StructType, const TSharedPtr<FJsonObject>& JsonObject, FString& OutError) const;
	};
}
