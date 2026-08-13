// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// Interchange import/export coverage 鈥?Layer A (orchestration), batch 1.
//
// Interchange is the unified UE import/export framework (FBX, glTF, OBJ, USD,
// MaterialX, OpenVDB, AxF). It exposes 1184 BlueprintCallable entry points on
// UE 5.7 and 1374 on UE 5.8, and SOMOLMCP covered none of them before this file.
// `InterchangeCore` and `InterchangeEngine` are Source/Runtime modules that ship
// with every supported engine from 5.3 up, including Installed/Rocket builds, so
// this family lights up on all six engines rather than only the newest.
//
// Version handling: the manager API moved only at two boundaries, verified
// against the actual headers of all six engines (see
// docs/SOMOLMCP_INTERCHANGE_API_INVENTORY.md section 3):
//   5.5 鈥?ImportAsset gained an OutImportedObjects overload, the async scripted
//         entry points appeared, and GetSupportedFormatsForObject took a second
//         (non-defaulted) SourceFileIndex parameter.
//   5.6 鈥?ReimportAsset / ScriptedReimportAssetAsync appeared.
// Everything else is source-compatible across 5.3鈥?.8 because the newer
// parameters carry defaults. Tools that need a gated API stay registered and
// report NOT_AVAILABLE_ON_ENGINE rather than silently disappearing.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "UObject/SoftObjectPath.h"
#include "Runtime/Launch/Resources/Version.h"

#if defined(SOMOLMCP_HAS_INTERCHANGEENGINE) && SOMOLMCP_HAS_INTERCHANGEENGINE
#define SOMOLMCP_WITH_INTERCHANGE 1
#else
// The L1 build gate did not find the module. Keep the family registered so
// clients get a typed capability answer instead of a missing-tool error.
#define SOMOLMCP_WITH_INTERCHANGE 0
#endif

#if SOMOLMCP_WITH_INTERCHANGE
#include "InterchangeManager.h"
#include "InterchangeSourceData.h"
#include "InterchangeTranslatorBase.h"
#endif

// 鈹€鈹€ engine API availability, measured from the shipped headers 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
#define SOMOLMCP_IX_HAS_ASYNC        (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5))
#define SOMOLMCP_IX_HAS_OUT_OBJECTS  (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5))
#define SOMOLMCP_IX_FORMATS_FOR_OBJECT_TAKES_INDEX \
                                     (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5))
#define SOMOLMCP_IX_HAS_REIMPORT     (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6))
#define SOMOLMCP_IX_HAS_DEST_NAME    (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
#define SOMOLMCP_IX_HAS_FORCE_DIALOG (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5))
// EInterchangeTranslatorAssetType gained Sounds and Grooms in 5.7; 5.3-5.6 stop at
// None/Textures/Materials/Meshes/Animations.
#define SOMOLMCP_IX_HAS_SOUND_GROOM_TYPES \
                                     (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7))

namespace UE::SOMOLMCP
{
namespace InterchangeToolsPrivate
{
	// These helpers are inline rather than static on purpose: which of them a
	// translation unit actually calls depends on the engine version gates, so a
	// static one would be an unreferenced local function (MSVC C4505) on the
	// engines where its branch is preprocessed out.
	static const TCHAR* const ErrEngine = TEXT("NOT_AVAILABLE_ON_ENGINE");

	inline FString EngineVersionString()
	{
		return FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	}

	/** Uniform refusal for an API the running engine does not ship. */
	inline bool RefuseUnavailable(
		const TSharedRef<FJsonObject>& OutStructured,
		FString& OutError,
		const TCHAR* ToolName,
		const TCHAR* RequiredApi,
		const TCHAR* MinEngine,
		const TCHAR* Alternative)
	{
		SololmcpError::Set(OutStructured, ErrEngine, TEXT(""),
			FString::Printf(TEXT("Requires UE %s or newer. On UE %s use '%s' instead."),
				MinEngine, *EngineVersionString(), Alternative));
		OutStructured->SetStringField(TEXT("tool"), ToolName);
		OutStructured->SetStringField(TEXT("required_api"), RequiredApi);
		OutStructured->SetStringField(TEXT("minimum_engine"), MinEngine);
		OutStructured->SetStringField(TEXT("engine_version"), EngineVersionString());
		OutStructured->SetStringField(TEXT("suggested_alternative"), Alternative);
		OutStructured->SetBoolField(TEXT("ok"), false);
		OutError = FString::Printf(TEXT("%s requires UE %s (running %s)."),
			ToolName, MinEngine, *EngineVersionString());
		return false;
	}

	/** Uniform refusal when the whole module is absent from the build. */
	inline bool RefuseNoModule(
		const TSharedRef<FJsonObject>& OutStructured,
		FString& OutError,
		const TCHAR* ToolName)
	{
		SololmcpError::Set(OutStructured, ErrEngine, TEXT(""),
			TEXT("This build was configured without InterchangeCore/InterchangeEngine. "
				 "Run interchange_capability_probe for the resolved module state."));
		OutStructured->SetStringField(TEXT("tool"), ToolName);
		OutStructured->SetStringField(TEXT("required_module"), TEXT("InterchangeEngine"));
		OutStructured->SetStringField(TEXT("engine_version"), EngineVersionString());
		OutStructured->SetBoolField(TEXT("ok"), false);
		OutError = TEXT("Interchange modules are not available in this build.");
		return false;
	}

	inline void AddStringArray(
		const TSharedRef<FJsonObject>& Object,
		const TCHAR* Field,
		const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Json;
		Json.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Json.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(Field, Json);
	}

#if SOMOLMCP_WITH_INTERCHANGE
	inline UInterchangeManager& Manager()
	{
		return UInterchangeManager::GetInterchangeManager();
	}

	/** Resolve a /Game/... path (or any object path) to a loaded UObject. */
	inline UObject* ResolveObject(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		return FSoftObjectPath(ObjectPath).TryLoad();
	}

	/**
	 * Build a source data wrapper for a file on disk. Interchange itself does not
	 * verify the file exists, and a missing file surfaces much later as an opaque
	 * translator failure, so the existence check happens here.
	 */
	inline UInterchangeSourceData* MakeSourceData(
		const FString& SourceFile,
		const TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		if (SourceFile.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("source_file"));
			OutError = TEXT("Missing source_file.");
			return nullptr;
		}
		const FString AbsolutePath = FPaths::ConvertRelativePathToFull(SourceFile);
		if (!IFileManager::Get().FileExists(*AbsolutePath))
		{
			SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("source_file"),
				FString::Printf(TEXT("Source file does not exist: %s"), *AbsolutePath));
			OutStructured->SetStringField(TEXT("source_file"), AbsolutePath);
			OutError = FString::Printf(TEXT("Source file not found: %s"), *AbsolutePath);
			return nullptr;
		}
		UInterchangeSourceData* SourceData = UInterchangeManager::CreateSourceData(AbsolutePath);
		if (SourceData == nullptr)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("source_file"),
				TEXT("UInterchangeManager::CreateSourceData returned null."));
			OutError = TEXT("CreateSourceData failed.");
		}
		return SourceData;
	}

	inline EInterchangeTranslatorType ParseTranslatorType(const FString& Value)
	{
		if (Value.Equals(TEXT("assets"), ESearchCase::IgnoreCase)) { return EInterchangeTranslatorType::Assets; }
		if (Value.Equals(TEXT("actors"), ESearchCase::IgnoreCase)) { return EInterchangeTranslatorType::Actors; }
		if (Value.Equals(TEXT("scenes"), ESearchCase::IgnoreCase)) { return EInterchangeTranslatorType::Scenes; }
		return EInterchangeTranslatorType::Invalid;
	}

	inline EInterchangeTranslatorAssetType ParseAssetType(const FString& Value)
	{
		if (Value.Equals(TEXT("textures"), ESearchCase::IgnoreCase))   { return EInterchangeTranslatorAssetType::Textures; }
		if (Value.Equals(TEXT("materials"), ESearchCase::IgnoreCase))  { return EInterchangeTranslatorAssetType::Materials; }
		if (Value.Equals(TEXT("meshes"), ESearchCase::IgnoreCase))     { return EInterchangeTranslatorAssetType::Meshes; }
		if (Value.Equals(TEXT("animations"), ESearchCase::IgnoreCase)) { return EInterchangeTranslatorAssetType::Animations; }
#if SOMOLMCP_IX_HAS_SOUND_GROOM_TYPES
		if (Value.Equals(TEXT("sounds"), ESearchCase::IgnoreCase))     { return EInterchangeTranslatorAssetType::Sounds; }
		if (Value.Equals(TEXT("grooms"), ESearchCase::IgnoreCase))     { return EInterchangeTranslatorAssetType::Grooms; }
#endif
		return EInterchangeTranslatorAssetType::None;
	}
#endif // SOMOLMCP_WITH_INTERCHANGE

	/**
	 * Asset-type names this engine actually accepts. Advertising sounds/grooms in the
	 * schema on 5.3-5.6 would offer the caller a value the enum cannot represent, so
	 * the list tracks the engine instead of being hardcoded.
	 */
	inline TArray<FString> AssetTypeEnumValues()
	{
		TArray<FString> Values = {
			TEXT("textures"), TEXT("materials"), TEXT("meshes"), TEXT("animations")
		};
#if SOMOLMCP_IX_HAS_SOUND_GROOM_TYPES
		Values.Add(TEXT("sounds"));
		Values.Add(TEXT("grooms"));
#endif
		return Values;
	}

#if SOMOLMCP_WITH_INTERCHANGE

	/**
	 * Populate FImportAssetParameters from tool arguments. Fields that only exist
	 * on newer engines are filled behind their own gate so one call site serves
	 * every supported version.
	 */
	inline void FillImportParameters(
		FImportAssetParameters& Parameters,
		const TSharedRef<FJsonObject>& Args,
		const TSharedRef<FJsonObject>& OutStructured)
	{
		bool bAutomated = true;
		Args->TryGetBoolField(TEXT("automated"), bAutomated);
		Parameters.bIsAutomated = bAutomated;

		bool bFollowRedirectors = false;
		if (Args->TryGetBoolField(TEXT("follow_redirectors"), bFollowRedirectors))
		{
			Parameters.bFollowRedirectors = bFollowRedirectors;
		}

		int32 ReimportSourceIndex = INDEX_NONE;
		if (Args->TryGetNumberField(TEXT("reimport_source_index"), ReimportSourceIndex))
		{
			Parameters.ReimportSourceIndex = ReimportSourceIndex;
		}

		const TArray<TSharedPtr<FJsonValue>>* OverridePipelines = nullptr;
		if (Args->TryGetArrayField(TEXT("override_pipelines"), OverridePipelines) && OverridePipelines != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *OverridePipelines)
			{
				FString PipelinePath;
				if (Value.IsValid() && Value->TryGetString(PipelinePath) && !PipelinePath.IsEmpty())
				{
					Parameters.OverridePipelines.Add(FSoftObjectPath(PipelinePath));
				}
			}
		}

		TArray<FString> AppliedOnlyOnNewerEngines;

		FString DestinationName;
		const bool bWantsDestinationName = Args->TryGetStringField(TEXT("destination_name"), DestinationName)
			&& !DestinationName.IsEmpty();
		bool bReplaceExisting = true;
		const bool bWantsReplaceExisting = Args->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
#if SOMOLMCP_IX_HAS_DEST_NAME
		if (bWantsDestinationName)
		{
			Parameters.DestinationName = DestinationName;
		}
		if (bWantsReplaceExisting)
		{
			Parameters.bReplaceExisting = bReplaceExisting;
		}
#else
		if (bWantsDestinationName)   { AppliedOnlyOnNewerEngines.Add(TEXT("destination_name")); }
		if (bWantsReplaceExisting)   { AppliedOnlyOnNewerEngines.Add(TEXT("replace_existing")); }
#endif

		bool bForceShowDialog = false;
		const bool bWantsForceShowDialog = Args->TryGetBoolField(TEXT("force_show_dialog"), bForceShowDialog);
#if SOMOLMCP_IX_HAS_FORCE_DIALOG
		if (bWantsForceShowDialog)
		{
			Parameters.bForceShowDialog = bForceShowDialog;
		}
#else
		if (bWantsForceShowDialog) { AppliedOnlyOnNewerEngines.Add(TEXT("force_show_dialog")); }
#endif

		// Silently dropping a caller-supplied parameter would make an import look
		// successful while ignoring what was asked, so report it explicitly.
		if (AppliedOnlyOnNewerEngines.Num() > 0)
		{
			AddStringArray(OutStructured, TEXT("ignored_parameters"), AppliedOnlyOnNewerEngines);
			OutStructured->SetStringField(TEXT("ignored_parameters_reason"),
				FString::Printf(TEXT("FImportAssetParameters on UE %s has no such field."),
					*EngineVersionString()));
		}
	}

	/** Run an import and report the resulting objects where the engine can supply them. */
	inline bool RunImportAsset(
		const FString& ContentPath,
		UInterchangeSourceData* SourceData,
		const FImportAssetParameters& Parameters,
		const TSharedRef<FJsonObject>& OutStructured)
	{
#if SOMOLMCP_IX_HAS_OUT_OBJECTS
		TArray<UObject*> ImportedObjects;
		const bool bImported = Manager().ImportAsset(ContentPath, SourceData, Parameters, ImportedObjects);

		TArray<FString> ImportedPaths;
		ImportedPaths.Reserve(ImportedObjects.Num());
		for (const UObject* Object : ImportedObjects)
		{
			if (Object != nullptr)
			{
				ImportedPaths.Add(Object->GetPathName());
			}
		}
		AddStringArray(OutStructured, TEXT("imported_objects"), ImportedPaths);
		OutStructured->SetNumberField(TEXT("imported_object_count"), ImportedPaths.Num());
		OutStructured->SetBoolField(TEXT("imported_objects_reported"), true);
		return bImported;
#else
		// UE 5.3/5.4 have no OutImportedObjects overload. The import still runs;
		// the caller is told the object list is unavailable rather than being
		// handed an empty array that reads as "nothing was imported".
		const bool bImported = Manager().ImportAsset(ContentPath, SourceData, Parameters);
		OutStructured->SetBoolField(TEXT("imported_objects_reported"), false);
		OutStructured->SetStringField(TEXT("imported_objects_note"),
			FString::Printf(
				TEXT("UE %s ImportAsset has no OutImportedObjects overload (added in 5.5). ")
				TEXT("Use asset_list on '%s' to enumerate the result."),
				*EngineVersionString(), *ContentPath));
		return bImported;
#endif
	}
#endif // SOMOLMCP_WITH_INTERCHANGE

	// 鈹€鈹€ shared schema fragments 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	inline TSharedRef<FJsonObject> ImportArgumentSchema(const bool bSceneImport)
	{
		TMap<FString, TSharedRef<FJsonObject>> Properties;
		Properties.Add(TEXT("source_file"), FSololmcpSchemaBuilder::String(
			TEXT("Absolute or project-relative path to the file to import (fbx, gltf, glb, obj, usd, ...).")));
		Properties.Add(TEXT("content_path"), FSololmcpSchemaBuilder::String(
			bSceneImport
				? TEXT("Destination content path for assets created by the scene import, e.g. /Game/Imported.")
				: TEXT("Destination content path, e.g. /Game/Imported/Props.")));
		Properties.Add(TEXT("automated"), FSololmcpSchemaBuilder::WithDefaultBoolean(
			FSololmcpSchemaBuilder::Boolean(
				TEXT("Suppress the modal import dialog. Defaults to true; leave it true for agent use.")),
			true));
		Properties.Add(TEXT("follow_redirectors"), FSololmcpSchemaBuilder::Boolean(
			TEXT("Follow redirectors when resolving the destination.")));
		Properties.Add(TEXT("destination_name"), FSololmcpSchemaBuilder::String(
			TEXT("Custom asset name for the import. Ignored on UE 5.3.")));
		Properties.Add(TEXT("replace_existing"), FSololmcpSchemaBuilder::Boolean(
			TEXT("Overwrite an existing asset at the destination. Ignored on UE 5.3.")));
		Properties.Add(TEXT("force_show_dialog"), FSololmcpSchemaBuilder::Boolean(
			TEXT("Force the import dialog even when automated. Ignored below UE 5.5.")));
		Properties.Add(TEXT("reimport_source_index"), FSololmcpSchemaBuilder::Integer(
			TEXT("Source file index for assets with multiple source files.")));
		Properties.Add(TEXT("override_pipelines"), FSololmcpSchemaBuilder::Array(
			FSololmcpSchemaBuilder::String(TEXT("Object path of an Interchange pipeline asset.")),
			TEXT("Use this exact pipeline stack instead of the project/user default.")));

		return FSololmcpSchemaBuilder::Object(Properties, {TEXT("source_file"), TEXT("content_path")});
	}
} // namespace InterchangeToolsPrivate

void RegisterInterchangeTools(FSololmcpToolRegistry& Registry)
{
	using namespace InterchangeToolsPrivate;

	// 鈹€鈹€ interchange_capability_probe 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_capability_probe"),
		TEXT("Report Interchange availability on the running engine: module presence, engine version, "
			 "and which gated APIs (async import, reimport, imported-object reporting) are usable. "
			 "Call this before branching on Interchange behaviour."),
		FSololmcpSchemaBuilder::Object({}, {}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			OutStructured->SetStringField(TEXT("engine_version"), EngineVersionString());
			OutStructured->SetBoolField(TEXT("module_available"), SOMOLMCP_WITH_INTERCHANGE ? true : false);

			TSharedRef<FJsonObject> Apis = MakeShared<FJsonObject>();
			Apis->SetBoolField(TEXT("import_asset"), SOMOLMCP_WITH_INTERCHANGE ? true : false);
			Apis->SetBoolField(TEXT("import_scene"), SOMOLMCP_WITH_INTERCHANGE ? true : false);
			Apis->SetBoolField(TEXT("export_asset"), SOMOLMCP_WITH_INTERCHANGE ? true : false);
			Apis->SetBoolField(TEXT("imported_objects_reported"), SOMOLMCP_IX_HAS_OUT_OBJECTS ? true : false);
			Apis->SetBoolField(TEXT("import_async"), SOMOLMCP_IX_HAS_ASYNC ? true : false);
			Apis->SetBoolField(TEXT("reimport"), SOMOLMCP_IX_HAS_REIMPORT ? true : false);
			Apis->SetBoolField(TEXT("formats_for_object"), SOMOLMCP_WITH_INTERCHANGE ? true : false);
			Apis->SetBoolField(TEXT("destination_name_parameter"), SOMOLMCP_IX_HAS_DEST_NAME ? true : false);
			Apis->SetBoolField(TEXT("force_show_dialog_parameter"), SOMOLMCP_IX_HAS_FORCE_DIALOG ? true : false);
			OutStructured->SetObjectField(TEXT("apis"), Apis);

#if SOMOLMCP_WITH_INTERCHANGE
			const TArray<FString> AssetFormats = Manager().GetSupportedFormats(EInterchangeTranslatorType::Assets);
			const TArray<FString> SceneFormats = Manager().GetSupportedFormats(EInterchangeTranslatorType::Scenes);
			OutStructured->SetNumberField(TEXT("supported_asset_format_count"), AssetFormats.Num());
			OutStructured->SetNumberField(TEXT("supported_scene_format_count"), SceneFormats.Num());
			OutSummary = FString::Printf(
				TEXT("Interchange available on UE %s: %d asset formats, %d scene formats, async=%s, reimport=%s."),
				*EngineVersionString(), AssetFormats.Num(), SceneFormats.Num(),
				SOMOLMCP_IX_HAS_ASYNC ? TEXT("yes") : TEXT("no"),
				SOMOLMCP_IX_HAS_REIMPORT ? TEXT("yes") : TEXT("no"));
#else
			OutSummary = FString::Printf(
				TEXT("Interchange modules were not compiled into this build (UE %s)."),
				*EngineVersionString());
#endif
			OutStructured->SetBoolField(TEXT("ok"), true);
			return true;
		},
		nullptr,
		30
	});

	// 鈹€鈹€ interchange_supported_formats_list 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_supported_formats_list"),
		TEXT("List file formats Interchange can translate, filtered by translator type "
			 "(assets, actors, scenes). Each entry is 'extension;Description'."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("translator_type"), FSololmcpSchemaBuilder::WithDefaultString(
					FSololmcpSchemaBuilder::String(
						TEXT("Which translator class to query."),
						{TEXT("assets"), TEXT("actors"), TEXT("scenes")}),
					TEXT("assets"))}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_supported_formats_list"));
#else
			FString TypeName = TEXT("assets");
			Args->TryGetStringField(TEXT("translator_type"), TypeName);
			const EInterchangeTranslatorType TranslatorType = ParseTranslatorType(TypeName);
			if (TranslatorType == EInterchangeTranslatorType::Invalid)
			{
				SololmcpError::InvalidType(OutStructured, TEXT("translator_type"), TEXT("assets|actors|scenes"));
				OutError = FString::Printf(TEXT("Unknown translator_type '%s'."), *TypeName);
				return false;
			}

			const TArray<FString> Formats = Manager().GetSupportedFormats(TranslatorType);
			AddStringArray(OutStructured, TEXT("formats"), Formats);
			OutStructured->SetNumberField(TEXT("format_count"), Formats.Num());
			OutStructured->SetStringField(TEXT("translator_type"), TypeName.ToLower());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d Interchange formats for translator type '%s'."),
				Formats.Num(), *TypeName.ToLower());
			return true;
#endif
		},
		nullptr,
		60
	});

	// 鈹€鈹€ interchange_supported_asset_type_formats_list 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_supported_asset_type_formats_list"),
		TEXT("List file formats Interchange can translate into a specific asset type "
			 "(textures, materials, meshes, animations, sounds, grooms)."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_type"), FSololmcpSchemaBuilder::String(
					TEXT("Asset type to query. sounds and grooms exist only on UE 5.7 and newer."),
					AssetTypeEnumValues())}
			},
			{TEXT("asset_type")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_supported_asset_type_formats_list"));
#else
			FString AssetTypeName;
			if (!Args->TryGetStringField(TEXT("asset_type"), AssetTypeName) || AssetTypeName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_type"));
				OutError = TEXT("Missing asset_type.");
				return false;
			}
			const EInterchangeTranslatorAssetType AssetType = ParseAssetType(AssetTypeName);
			if (AssetType == EInterchangeTranslatorAssetType::None)
			{
				const FString Accepted = FString::Join(AssetTypeEnumValues(), TEXT("|"));
				SololmcpError::InvalidType(OutStructured, TEXT("asset_type"), Accepted);
				AddStringArray(OutStructured, TEXT("accepted_asset_types"), AssetTypeEnumValues());
				OutStructured->SetStringField(TEXT("engine_version"), EngineVersionString());
				OutError = FString::Printf(TEXT("Unknown asset_type '%s' on UE %s. Accepted: %s."),
					*AssetTypeName, *EngineVersionString(), *Accepted);
				return false;
			}

			// The 5.5+ overload adds two defaulted parameters, so this single-argument
			// call compiles unchanged on 5.3 through 5.8.
			const TArray<FString> Formats = Manager().GetSupportedAssetTypeFormats(AssetType);
			AddStringArray(OutStructured, TEXT("formats"), Formats);
			OutStructured->SetNumberField(TEXT("format_count"), Formats.Num());
			OutStructured->SetStringField(TEXT("asset_type"), AssetTypeName.ToLower());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d Interchange formats produce '%s'."),
				Formats.Num(), *AssetTypeName.ToLower());
			return true;
#endif
		},
		nullptr,
		60
	});

	// 鈹€鈹€ interchange_supported_formats_for_object 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_supported_formats_for_object"),
		TEXT("List the source formats that can reimport an existing asset. "
			 "source_file_index selects among multi-source assets on UE 5.5 and newer."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("object_path"), FSololmcpSchemaBuilder::String(
					TEXT("Object path of the asset, e.g. /Game/Meshes/SM_Rock.SM_Rock."))},
				{TEXT("source_file_index"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(
						TEXT("Source file index. Ignored on UE 5.3/5.4, which have no such parameter.")),
					0)}
			},
			{TEXT("object_path")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_supported_formats_for_object"));
#else
			FString ObjectPath;
			if (!Args->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("object_path"));
				OutError = TEXT("Missing object_path.");
				return false;
			}
			UObject* Object = ResolveObject(ObjectPath);
			if (Object == nullptr)
			{
				SololmcpError::InvalidPath(OutStructured, ObjectPath);
				OutError = FString::Printf(TEXT("Could not load object '%s'."), *ObjectPath);
				return false;
			}

			int32 SourceFileIndex = 0;
			Args->TryGetNumberField(TEXT("source_file_index"), SourceFileIndex);

#if SOMOLMCP_IX_FORMATS_FOR_OBJECT_TAKES_INDEX
			const TArray<FString> Formats = Manager().GetSupportedFormatsForObject(Object, SourceFileIndex);
			OutStructured->SetNumberField(TEXT("source_file_index"), SourceFileIndex);
#else
			const TArray<FString> Formats = Manager().GetSupportedFormatsForObject(Object);
			if (SourceFileIndex != 0)
			{
				OutStructured->SetStringField(TEXT("ignored_parameters_reason"),
					FString::Printf(
						TEXT("UE %s GetSupportedFormatsForObject takes no source file index; index %d was ignored."),
						*EngineVersionString(), SourceFileIndex));
				AddStringArray(OutStructured, TEXT("ignored_parameters"), {TEXT("source_file_index")});
			}
#endif
			AddStringArray(OutStructured, TEXT("formats"), Formats);
			OutStructured->SetNumberField(TEXT("format_count"), Formats.Num());
			OutStructured->SetStringField(TEXT("object_path"), Object->GetPathName());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d source formats can reimport '%s'."),
				Formats.Num(), *Object->GetName());
			return true;
#endif
		},
		nullptr,
		30
	});

	// 鈹€鈹€ interchange_source_data_create 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_source_data_create"),
		TEXT("Validate a source file and report what Interchange can do with it: whether a translator "
			 "accepts it, and its resolved absolute path and size. Read-only; nothing is imported."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("source_file"), FSololmcpSchemaBuilder::String(
					TEXT("Absolute or project-relative path to the source file."))}
			},
			{TEXT("source_file")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_source_data_create"));
#else
			FString SourceFile;
			Args->TryGetStringField(TEXT("source_file"), SourceFile);
			UInterchangeSourceData* SourceData = MakeSourceData(SourceFile, OutStructured, OutError);
			if (SourceData == nullptr)
			{
				return false;
			}

			const FString AbsolutePath = FPaths::ConvertRelativePathToFull(SourceFile);
			const bool bCanTranslate = Manager().CanTranslateSourceData(SourceData);
			OutStructured->SetStringField(TEXT("source_file"), AbsolutePath);
			OutStructured->SetStringField(TEXT("extension"), FPaths::GetExtension(AbsolutePath));
			OutStructured->SetNumberField(TEXT("file_size_bytes"),
				static_cast<double>(IFileManager::Get().FileSize(*AbsolutePath)));
			OutStructured->SetBoolField(TEXT("can_translate"), bCanTranslate);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("'%s' %s be translated by Interchange."),
				*FPaths::GetCleanFilename(AbsolutePath), bCanTranslate ? TEXT("can") : TEXT("cannot"));
			return true;
#endif
		},
		nullptr,
		0
	});

	// 鈹€鈹€ interchange_can_translate 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_can_translate"),
		TEXT("Check whether any registered Interchange translator accepts a source file. "
			 "Use before interchange_import_asset to fail fast on unsupported formats."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("source_file"), FSololmcpSchemaBuilder::String(TEXT("Path to the source file."))}
			},
			{TEXT("source_file")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_can_translate"));
#else
			FString SourceFile;
			Args->TryGetStringField(TEXT("source_file"), SourceFile);
			UInterchangeSourceData* SourceData = MakeSourceData(SourceFile, OutStructured, OutError);
			if (SourceData == nullptr)
			{
				return false;
			}
			const bool bCanTranslate = Manager().CanTranslateSourceData(SourceData);
			OutStructured->SetBoolField(TEXT("can_translate"), bCanTranslate);
			OutStructured->SetStringField(TEXT("source_file"), FPaths::ConvertRelativePathToFull(SourceFile));
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = bCanTranslate
				? TEXT("A translator accepts this source file.")
				: TEXT("No registered translator accepts this source file.");
			return true;
#endif
		},
		nullptr,
		0
	});

	// 鈹€鈹€ interchange_can_reimport 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_can_reimport"),
		TEXT("Check whether an existing asset can be reimported through Interchange, and return the "
			 "source filenames recorded on it."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("object_path"), FSololmcpSchemaBuilder::String(TEXT("Object path of the asset."))}
			},
			{TEXT("object_path")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_can_reimport"));
#else
			FString ObjectPath;
			if (!Args->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("object_path"));
				OutError = TEXT("Missing object_path.");
				return false;
			}
			UObject* Object = ResolveObject(ObjectPath);
			if (Object == nullptr)
			{
				SololmcpError::InvalidPath(OutStructured, ObjectPath);
				OutError = FString::Printf(TEXT("Could not load object '%s'."), *ObjectPath);
				return false;
			}

			TArray<FString> Filenames;
			const bool bCanReimport = Manager().CanReimport(Object, Filenames);
			OutStructured->SetBoolField(TEXT("can_reimport"), bCanReimport);
			AddStringArray(OutStructured, TEXT("source_filenames"), Filenames);
			OutStructured->SetNumberField(TEXT("source_file_count"), Filenames.Num());
			OutStructured->SetStringField(TEXT("object_path"), Object->GetPathName());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("'%s' %s reimportable (%d source file(s))."),
				*Object->GetName(), bCanReimport ? TEXT("is") : TEXT("is not"), Filenames.Num());
			return true;
#endif
		},
		nullptr,
		0
	});

	// 鈹€鈹€ interchange_registered_factory_class_get 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_registered_factory_class_get"),
		TEXT("Resolve which Interchange factory class is registered to produce a given asset class. "
			 "Useful for diagnosing why an import produces an unexpected asset type."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("class_path"), FSololmcpSchemaBuilder::String(
					TEXT("Class path of the asset type, e.g. /Script/Engine.StaticMesh."))}
			},
			{TEXT("class_path")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_registered_factory_class_get"));
#else
			FString ClassPath;
			if (!Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("class_path"));
				OutError = TEXT("Missing class_path.");
				return false;
			}
			UClass* ClassToMake = Cast<UClass>(FSoftObjectPath(ClassPath).TryLoad());
			if (ClassToMake == nullptr)
			{
				SololmcpError::NotFound(OutStructured, ClassPath);
				OutError = FString::Printf(TEXT("Could not resolve class '%s'."), *ClassPath);
				return false;
			}

			const UClass* FactoryClass = Manager().GetRegisteredFactoryClass(ClassToMake);
			OutStructured->SetStringField(TEXT("class_path"), ClassToMake->GetPathName());
			OutStructured->SetBoolField(TEXT("has_factory"), FactoryClass != nullptr);
			OutStructured->SetStringField(TEXT("factory_class"),
				FactoryClass != nullptr ? FactoryClass->GetPathName() : FString());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FactoryClass != nullptr
				? FString::Printf(TEXT("'%s' is produced by %s."), *ClassToMake->GetName(), *FactoryClass->GetName())
				: FString::Printf(TEXT("No Interchange factory is registered for '%s'."), *ClassToMake->GetName());
			return true;
#endif
		},
		nullptr,
		60
	});

	// 鈹€鈹€ interchange_import_asset 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_import_asset"),
		TEXT("Import a source file into the content browser through Interchange (fbx, gltf, glb, obj, "
			 "usd, MaterialX, and every other registered translator). Runs synchronously and returns "
			 "the created objects on UE 5.5 and newer."),
		ImportArgumentSchema(/*bSceneImport=*/false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_import_asset"));
#else
			FString SourceFile;
			Args->TryGetStringField(TEXT("source_file"), SourceFile);
			FString ContentPath;
			if (!Args->TryGetStringField(TEXT("content_path"), ContentPath) || ContentPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("content_path"));
				OutError = TEXT("Missing content_path.");
				return false;
			}
			UInterchangeSourceData* SourceData = MakeSourceData(SourceFile, OutStructured, OutError);
			if (SourceData == nullptr)
			{
				return false;
			}

			FImportAssetParameters Parameters;
			FillImportParameters(Parameters, Args, OutStructured);

			const bool bImported = RunImportAsset(ContentPath, SourceData, Parameters, OutStructured);
			OutStructured->SetStringField(TEXT("source_file"), FPaths::ConvertRelativePathToFull(SourceFile));
			OutStructured->SetStringField(TEXT("content_path"), ContentPath);
			OutStructured->SetBoolField(TEXT("ok"), bImported);
			if (!bImported)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("source_file"),
					TEXT("Interchange refused the import. Run interchange_can_translate to confirm a "
						 "translator accepts this file, and check the Output Log for translator messages."));
				OutError = FString::Printf(TEXT("Interchange import failed for '%s'."), *SourceFile);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Imported '%s' into %s."),
				*FPaths::GetCleanFilename(SourceFile), *ContentPath);
			return true;
#endif
		},
		nullptr,
		0
	});

	// 鈹€鈹€ interchange_import_scene 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_import_scene"),
		TEXT("Import a source file as a scene through Interchange, spawning actors into the current "
			 "level in addition to creating assets."),
		ImportArgumentSchema(/*bSceneImport=*/true),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_import_scene"));
#else
			FString SourceFile;
			Args->TryGetStringField(TEXT("source_file"), SourceFile);
			FString ContentPath;
			if (!Args->TryGetStringField(TEXT("content_path"), ContentPath) || ContentPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("content_path"));
				OutError = TEXT("Missing content_path.");
				return false;
			}
			UInterchangeSourceData* SourceData = MakeSourceData(SourceFile, OutStructured, OutError);
			if (SourceData == nullptr)
			{
				return false;
			}

			FImportAssetParameters Parameters;
			FillImportParameters(Parameters, Args, OutStructured);

			const bool bImported = Manager().ImportScene(ContentPath, SourceData, Parameters);
			OutStructured->SetStringField(TEXT("source_file"), FPaths::ConvertRelativePathToFull(SourceFile));
			OutStructured->SetStringField(TEXT("content_path"), ContentPath);
			OutStructured->SetBoolField(TEXT("ok"), bImported);
			if (!bImported)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("source_file"),
					TEXT("Interchange refused the scene import. Confirm the format supports scene import "
						 "with interchange_supported_formats_list translator_type=scenes."));
				OutError = FString::Printf(TEXT("Interchange scene import failed for '%s'."), *SourceFile);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Imported scene '%s' into %s."),
				*FPaths::GetCleanFilename(SourceFile), *ContentPath);
			return true;
#endif
		},
		nullptr,
		0
	});

	// 鈹€鈹€ interchange_import_asset_async (5.5+) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_import_asset_async"),
		TEXT("Queue an asset import without blocking the game thread. Requires UE 5.5 or newer; "
			 "on older engines use interchange_import_asset."),
		ImportArgumentSchema(/*bSceneImport=*/false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_import_asset_async"));
#elif !SOMOLMCP_IX_HAS_ASYNC
			return RefuseUnavailable(OutStructured, OutError,
				TEXT("interchange_import_asset_async"),
				TEXT("UInterchangeManager::ScriptedImportAssetAsync"),
				TEXT("5.5"), TEXT("interchange_import_asset"));
#else
			FString SourceFile;
			Args->TryGetStringField(TEXT("source_file"), SourceFile);
			FString ContentPath;
			if (!Args->TryGetStringField(TEXT("content_path"), ContentPath) || ContentPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("content_path"));
				OutError = TEXT("Missing content_path.");
				return false;
			}
			UInterchangeSourceData* SourceData = MakeSourceData(SourceFile, OutStructured, OutError);
			if (SourceData == nullptr)
			{
				return false;
			}

			FImportAssetParameters Parameters;
			FillImportParameters(Parameters, Args, OutStructured);

			const bool bQueued = Manager().ScriptedImportAssetAsync(ContentPath, SourceData, Parameters);
			OutStructured->SetStringField(TEXT("source_file"), FPaths::ConvertRelativePathToFull(SourceFile));
			OutStructured->SetStringField(TEXT("content_path"), ContentPath);
			OutStructured->SetBoolField(TEXT("queued"), bQueued);
			OutStructured->SetBoolField(TEXT("ok"), bQueued);
			if (!bQueued)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("source_file"),
					TEXT("Interchange refused to queue the import."));
				OutError = FString::Printf(TEXT("Async import was not queued for '%s'."), *SourceFile);
				return false;
			}
			OutSummary = FString::Printf(
				TEXT("Queued async import of '%s' into %s. Poll asset_list on the destination for completion."),
				*FPaths::GetCleanFilename(SourceFile), *ContentPath);
			return true;
#endif
		},
		nullptr,
		0
	});

	// 鈹€鈹€ interchange_import_scene_async (5.5+) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_import_scene_async"),
		TEXT("Queue a scene import without blocking the game thread. Requires UE 5.5 or newer; "
			 "on older engines use interchange_import_scene."),
		ImportArgumentSchema(/*bSceneImport=*/true),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_import_scene_async"));
#elif !SOMOLMCP_IX_HAS_ASYNC
			return RefuseUnavailable(OutStructured, OutError,
				TEXT("interchange_import_scene_async"),
				TEXT("UInterchangeManager::ScriptedImportSceneAsync"),
				TEXT("5.5"), TEXT("interchange_import_scene"));
#else
			FString SourceFile;
			Args->TryGetStringField(TEXT("source_file"), SourceFile);
			FString ContentPath;
			if (!Args->TryGetStringField(TEXT("content_path"), ContentPath) || ContentPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("content_path"));
				OutError = TEXT("Missing content_path.");
				return false;
			}
			UInterchangeSourceData* SourceData = MakeSourceData(SourceFile, OutStructured, OutError);
			if (SourceData == nullptr)
			{
				return false;
			}

			FImportAssetParameters Parameters;
			FillImportParameters(Parameters, Args, OutStructured);

			const bool bQueued = Manager().ScriptedImportSceneAsync(ContentPath, SourceData, Parameters);
			OutStructured->SetStringField(TEXT("content_path"), ContentPath);
			OutStructured->SetBoolField(TEXT("queued"), bQueued);
			OutStructured->SetBoolField(TEXT("ok"), bQueued);
			if (!bQueued)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("source_file"),
					TEXT("Interchange refused to queue the scene import."));
				OutError = FString::Printf(TEXT("Async scene import was not queued for '%s'."), *SourceFile);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Queued async scene import of '%s' into %s."),
				*FPaths::GetCleanFilename(SourceFile), *ContentPath);
			return true;
#endif
		},
		nullptr,
		0
	});

	// 鈹€鈹€ interchange_reimport_asset (5.6+) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_reimport_asset"),
		TEXT("Reimport an existing asset from its recorded source file through Interchange. "
			 "Requires UE 5.6 or newer."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("object_path"), FSololmcpSchemaBuilder::String(
					TEXT("Object path of the asset to reimport."))},
				{TEXT("reimport_source_index"), FSololmcpSchemaBuilder::Integer(
					TEXT("Which recorded source file to reimport from."))},
				{TEXT("automated"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Suppress the modal dialog.")), true)},
				{TEXT("override_pipelines"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::String(TEXT("Object path of an Interchange pipeline asset.")),
					TEXT("Use this exact pipeline stack for the reimport."))}
			},
			{TEXT("object_path")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_reimport_asset"));
#elif !SOMOLMCP_IX_HAS_REIMPORT
			return RefuseUnavailable(OutStructured, OutError,
				TEXT("interchange_reimport_asset"),
				TEXT("UInterchangeManager::ReimportAsset"),
				TEXT("5.6"),
				TEXT("interchange_import_asset with the original source_file and replace_existing=true"));
#else
			FString ObjectPath;
			if (!Args->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("object_path"));
				OutError = TEXT("Missing object_path.");
				return false;
			}
			UObject* Object = ResolveObject(ObjectPath);
			if (Object == nullptr)
			{
				SololmcpError::InvalidPath(OutStructured, ObjectPath);
				OutError = FString::Printf(TEXT("Could not load object '%s'."), *ObjectPath);
				return false;
			}

			FImportAssetParameters Parameters;
			FillImportParameters(Parameters, Args, OutStructured);
			Parameters.ReimportAsset = Object;

			TArray<UObject*> ReimportedObjects;
			const bool bReimported = Manager().ReimportAsset(Object, Parameters, ReimportedObjects);

			TArray<FString> ReimportedPaths;
			ReimportedPaths.Reserve(ReimportedObjects.Num());
			for (const UObject* Reimported : ReimportedObjects)
			{
				if (Reimported != nullptr)
				{
					ReimportedPaths.Add(Reimported->GetPathName());
				}
			}
			AddStringArray(OutStructured, TEXT("reimported_objects"), ReimportedPaths);
			OutStructured->SetNumberField(TEXT("reimported_object_count"), ReimportedPaths.Num());
			OutStructured->SetStringField(TEXT("object_path"), Object->GetPathName());
			OutStructured->SetBoolField(TEXT("ok"), bReimported);
			if (!bReimported)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("object_path"),
					TEXT("Reimport failed. Use interchange_can_reimport to confirm the asset has a usable "
						 "source file recorded."));
				OutError = FString::Printf(TEXT("Reimport failed for '%s'."), *ObjectPath);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Reimported '%s' (%d object(s) updated)."),
				*Object->GetName(), ReimportedPaths.Num());
			return true;
#endif
		},
		nullptr,
		0
	});

	// 鈹€鈹€ interchange_reimport_asset_async (5.6+) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_reimport_asset_async"),
		TEXT("Queue a reimport without blocking the game thread. Requires UE 5.6 or newer."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("object_path"), FSololmcpSchemaBuilder::String(
					TEXT("Object path of the asset to reimport."))},
				{TEXT("reimport_source_index"), FSololmcpSchemaBuilder::Integer(
					TEXT("Which recorded source file to reimport from."))},
				{TEXT("automated"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Suppress the modal dialog.")), true)}
			},
			{TEXT("object_path")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_reimport_asset_async"));
#elif !SOMOLMCP_IX_HAS_REIMPORT
			return RefuseUnavailable(OutStructured, OutError,
				TEXT("interchange_reimport_asset_async"),
				TEXT("UInterchangeManager::ScriptedReimportAssetAsync"),
				TEXT("5.6"), TEXT("interchange_import_asset"));
#else
			FString ObjectPath;
			if (!Args->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("object_path"));
				OutError = TEXT("Missing object_path.");
				return false;
			}
			UObject* Object = ResolveObject(ObjectPath);
			if (Object == nullptr)
			{
				SololmcpError::InvalidPath(OutStructured, ObjectPath);
				OutError = FString::Printf(TEXT("Could not load object '%s'."), *ObjectPath);
				return false;
			}

			FImportAssetParameters Parameters;
			FillImportParameters(Parameters, Args, OutStructured);
			Parameters.ReimportAsset = Object;

			const bool bQueued = Manager().ScriptedReimportAssetAsync(Object, Parameters);
			OutStructured->SetStringField(TEXT("object_path"), Object->GetPathName());
			OutStructured->SetBoolField(TEXT("queued"), bQueued);
			OutStructured->SetBoolField(TEXT("ok"), bQueued);
			if (!bQueued)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("object_path"),
					TEXT("Interchange refused to queue the reimport."));
				OutError = FString::Printf(TEXT("Async reimport was not queued for '%s'."), *ObjectPath);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Queued async reimport of '%s'."), *Object->GetName());
			return true;
#endif
		},
		nullptr,
		0
	});

	// 鈹€鈹€ interchange_export_asset 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_export_asset"),
		TEXT("Export an asset through Interchange. With automated=true no dialog is shown and the "
			 "export uses the registered default settings for the asset type."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("object_path"), FSololmcpSchemaBuilder::String(
					TEXT("Object path of the asset to export."))},
				{TEXT("automated"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Suppress the export dialog.")), true)}
			},
			{TEXT("object_path")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_export_asset"));
#else
			FString ObjectPath;
			if (!Args->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("object_path"));
				OutError = TEXT("Missing object_path.");
				return false;
			}
			UObject* Object = ResolveObject(ObjectPath);
			if (Object == nullptr)
			{
				SololmcpError::InvalidPath(OutStructured, ObjectPath);
				OutError = FString::Printf(TEXT("Could not load object '%s'."), *ObjectPath);
				return false;
			}

			bool bAutomated = true;
			Args->TryGetBoolField(TEXT("automated"), bAutomated);

			const bool bExported = Manager().ExportAsset(Object, bAutomated);
			OutStructured->SetStringField(TEXT("object_path"), Object->GetPathName());
			OutStructured->SetBoolField(TEXT("automated"), bAutomated);
			OutStructured->SetBoolField(TEXT("ok"), bExported);
			if (!bExported)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("object_path"),
					TEXT("Interchange has no registered exporter for this asset type, or the export was cancelled."));
				OutError = FString::Printf(TEXT("Export failed for '%s'."), *ObjectPath);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Exported '%s'."), *Object->GetName());
			return true;
#endif
		},
		nullptr,
		0
	});

	// 鈹€鈹€ interchange_export_scene 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
	Registry.Register({
		TEXT("interchange_export_scene"),
		TEXT("Export a world/level through Interchange. Pass the world object path, or omit it to "
			 "export the currently edited world."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("world_path"), FSololmcpSchemaBuilder::String(
					TEXT("Object path of the world to export. Omit to use the current editor world."))},
				{TEXT("automated"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Suppress the export dialog.")), true)}
			},
			{}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_export_scene"));
#else
			UObject* World = nullptr;
			FString WorldPath;
			if (Args->TryGetStringField(TEXT("world_path"), WorldPath) && !WorldPath.IsEmpty())
			{
				World = ResolveObject(WorldPath);
				if (World == nullptr)
				{
					SololmcpError::InvalidPath(OutStructured, WorldPath);
					OutError = FString::Printf(TEXT("Could not load world '%s'."), *WorldPath);
					return false;
				}
			}
			else
			{
				FString WorldError;
				World = Context.Services.GetEditorWorld(WorldError);
				if (World == nullptr)
				{
					SololmcpError::NotFound(OutStructured, TEXT("editor world"));
					OutError = WorldError.IsEmpty()
						? TEXT("No editor world is available; pass world_path explicitly.")
						: WorldError;
					return false;
				}
			}

			bool bAutomated = true;
			Args->TryGetBoolField(TEXT("automated"), bAutomated);

			const bool bExported = Manager().ExportScene(World, bAutomated);
			OutStructured->SetStringField(TEXT("world_path"), World->GetPathName());
			OutStructured->SetBoolField(TEXT("automated"), bAutomated);
			OutStructured->SetBoolField(TEXT("ok"), bExported);
			if (!bExported)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("world_path"),
					TEXT("Interchange has no registered scene exporter, or the export was cancelled."));
				OutError = FString::Printf(TEXT("Scene export failed for '%s'."), *World->GetPathName());
				return false;
			}
			OutSummary = FString::Printf(TEXT("Exported scene '%s'."), *World->GetName());
			return true;
#endif
		},
		nullptr,
		0
	});
}

} // namespace UE::SOMOLMCP
