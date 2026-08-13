// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// Interchange coverage — Layer B (pipeline configuration), batch 3.
//
// Pipelines are what decide how a translated graph becomes assets: which meshes
// combine, whether materials are imported, LOD and collision policy, texture
// compression, animation sampling. Nearly all of that surface is UPROPERTY rather
// than UFUNCTION, so unlike Layer A there is little to wrap one-to-one — the
// leverage is generic property reflection over the pipeline objects.
//
// Property writes go through FSololmcpEditorServices::ApplyPropertiesWithReceipts
// rather than a private reimplementation: it already handles nested struct paths
// and returns post-set readback receipts, which is exactly what a queued caller
// needs to confirm a write landed without issuing a second read.
//
// Queue notes (SOMOLMCP_COMPLETE_SOLUTION.md 6.4): _set_batch applies writes
// across many pipelines in one game-thread entry, and _catalog lets a client
// validate a whole planned wave before submitting any of it.
//
// UInterchangeProjectSettings, FInterchangeImportSettings, FInterchangePipelineStack
// and the GetDefaultImportSettings accessors were verified present and identically
// shaped on UE 5.3 through 5.8, so this family needs no engine version gates.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "Services/SololmcpEditorServices.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "Runtime/Launch/Resources/Version.h"

#if defined(SOMOLMCP_HAS_INTERCHANGEENGINE) && SOMOLMCP_HAS_INTERCHANGEENGINE
#define SOMOLMCP_WITH_INTERCHANGE_PIPELINES 1
#else
#define SOMOLMCP_WITH_INTERCHANGE_PIPELINES 0
#endif

#if SOMOLMCP_WITH_INTERCHANGE_PIPELINES
#include "InterchangeProjectSettings.h"
#include "InterchangePipelineBase.h"
#endif

namespace UE::SOMOLMCP
{
namespace InterchangePipelineToolsPrivate
{
	inline FString EngineVersionString()
	{
		return FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	}

	inline bool RefuseNoModule(
		const TSharedRef<FJsonObject>& OutStructured, FString& OutError, const TCHAR* ToolName)
	{
		SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE_ON_ENGINE"), TEXT(""),
			TEXT("This build was configured without InterchangeEngine."));
		OutStructured->SetStringField(TEXT("tool"), ToolName);
		OutStructured->SetStringField(TEXT("required_module"), TEXT("InterchangeEngine"));
		OutStructured->SetStringField(TEXT("engine_version"), EngineVersionString());
		OutStructured->SetBoolField(TEXT("ok"), false);
		OutError = TEXT("InterchangeEngine is not available in this build.");
		return false;
	}

#if SOMOLMCP_WITH_INTERCHANGE_PIPELINES
	/** Load a pipeline asset by object path. */
	inline UInterchangePipelineBase* LoadPipeline(
		const FString& PipelinePath,
		const TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		if (PipelinePath.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("pipeline_path"));
			OutError = TEXT("Missing pipeline_path.");
			return nullptr;
		}
		UObject* Loaded = FSoftObjectPath(PipelinePath).TryLoad();
		UInterchangePipelineBase* Pipeline = Cast<UInterchangePipelineBase>(Loaded);
		if (Pipeline == nullptr)
		{
			// A default-object path (…Default__X) or a blueprint wrapper resolves to
			// something that is not a pipeline; say which so the caller can correct it.
			SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("pipeline_path"),
				Loaded != nullptr
					? FString::Printf(TEXT("'%s' loaded as %s, which is not a UInterchangePipelineBase."),
						*PipelinePath, *Loaded->GetClass()->GetName())
					: FString::Printf(TEXT("Could not load '%s'."), *PipelinePath));
			OutStructured->SetStringField(TEXT("pipeline_path"), PipelinePath);
			OutError = FString::Printf(TEXT("'%s' is not an Interchange pipeline."), *PipelinePath);
		}
		return Pipeline;
	}

	/** Collect the editable properties of a class as catalog rows. */
	inline void BuildPropertyCatalog(
		const UClass* Class,
		TArray<TSharedPtr<FJsonValue>>& OutRows,
		int32& OutEditableCount)
	{
		OutEditableCount = 0;
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			const FProperty* Property = *It;
			const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit)
				&& !Property->HasAnyPropertyFlags(CPF_EditConst);

			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Property->GetName());
			Row->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			Row->SetBoolField(TEXT("editable"), bEditable);
			Row->SetStringField(TEXT("owner_class"),
				Property->GetOwnerClass() != nullptr ? Property->GetOwnerClass()->GetName() : FString());
			const FString Tooltip = Property->GetMetaData(TEXT("ToolTip"));
			if (!Tooltip.IsEmpty())
			{
				Row->SetStringField(TEXT("tooltip"), Tooltip.Left(400));
			}
			OutRows.Add(MakeShared<FJsonValueObject>(Row));
			if (bEditable)
			{
				++OutEditableCount;
			}
		}
	}
#endif // SOMOLMCP_WITH_INTERCHANGE_PIPELINES
} // namespace InterchangePipelineToolsPrivate

void RegisterInterchangePipelineTools(FSololmcpToolRegistry& Registry)
{
	using namespace InterchangePipelineToolsPrivate;

	// ── interchange_pipeline_stack_list ────────────────────────────────────
	Registry.Register({
		TEXT("interchange_pipeline_stack_list"),
		TEXT("List the Interchange pipeline stacks configured in Project Settings, for asset imports "
			 "and for scene imports, including which stack is the default. These are the stacks an "
			 "import uses when no override_pipelines are supplied."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("scene_import"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Query the scene-import settings instead of the asset-import settings.")),
					false)}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_PIPELINES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_pipeline_stack_list"));
#else
			bool bSceneImport = false;
			Args->TryGetBoolField(TEXT("scene_import"), bSceneImport);

			// The accessors live on FInterchangeProjectSettingsUtils, not on the
			// UInterchangeProjectSettings object itself. Both types are present and
			// identically shaped on 5.3 through 5.8.
			const FInterchangeImportSettings& Settings =
				FInterchangeProjectSettingsUtils::GetDefaultImportSettings(bSceneImport);

			TArray<TSharedPtr<FJsonValue>> Stacks;
			for (const TPair<FName, FInterchangePipelineStack>& Pair : Settings.PipelineStacks)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("stack_name"), Pair.Key.ToString());
				Row->SetNumberField(TEXT("pipeline_count"), Pair.Value.Pipelines.Num());
				Row->SetBoolField(TEXT("is_default"), Pair.Key == Settings.DefaultPipelineStack);

				TArray<FString> Paths;
				Paths.Reserve(Pair.Value.Pipelines.Num());
				for (const FSoftObjectPath& Path : Pair.Value.Pipelines)
				{
					Paths.Add(Path.ToString());
				}
				TArray<TSharedPtr<FJsonValue>> PathJson;
				for (const FString& Path : Paths)
				{
					PathJson.Add(MakeShared<FJsonValueString>(Path));
				}
				Row->SetArrayField(TEXT("pipelines"), PathJson);
				Stacks.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("stacks"), Stacks);
			OutStructured->SetNumberField(TEXT("stack_count"), Stacks.Num());
			OutStructured->SetStringField(TEXT("default_stack"), Settings.DefaultPipelineStack.ToString());
			OutStructured->SetBoolField(TEXT("scene_import"), bSceneImport);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d %s-import pipeline stack(s); default is '%s'."),
				Stacks.Num(), bSceneImport ? TEXT("scene") : TEXT("asset"),
				*Settings.DefaultPipelineStack.ToString());
			return true;
#endif
		},
		nullptr,
		30
	});

	// ── interchange_pipeline_inspect ───────────────────────────────────────
	Registry.Register({
		TEXT("interchange_pipeline_inspect"),
		TEXT("Dump a pipeline asset's current property values as JSON. Use this to see what an "
			 "import will actually do before running it."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("pipeline_path"), FSololmcpSchemaBuilder::String(
					TEXT("Object path of the pipeline asset, e.g. /Interchange/Pipelines/DefaultAssetsPipeline.DefaultAssetsPipeline."))}
			},
			{TEXT("pipeline_path")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_PIPELINES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_pipeline_inspect"));
#else
			FString PipelinePath;
			Args->TryGetStringField(TEXT("pipeline_path"), PipelinePath);
			UInterchangePipelineBase* Pipeline = LoadPipeline(PipelinePath, OutStructured, OutError);
			if (Pipeline == nullptr)
			{
				return false;
			}

			TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
			if (!FJsonObjectConverter::UStructToJsonObject(
					Pipeline->GetClass(), Pipeline, Values, /*CheckFlags=*/CPF_Edit, /*SkipFlags=*/0))
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("pipeline_path"),
					TEXT("Could not serialize the pipeline's editable properties."));
				OutError = TEXT("Pipeline serialization failed.");
				return false;
			}

			OutStructured->SetStringField(TEXT("pipeline_path"), Pipeline->GetPathName());
			OutStructured->SetStringField(TEXT("pipeline_class"), Pipeline->GetClass()->GetPathName());
			OutStructured->SetObjectField(TEXT("properties"), Values);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Inspected pipeline '%s' (%s)."),
				*Pipeline->GetName(), *Pipeline->GetClass()->GetName());
			return true;
#endif
		},
		nullptr,
		10
	});

	// ── interchange_pipeline_catalog ───────────────────────────────────────
	Registry.Register({
		TEXT("interchange_pipeline_catalog"),
		TEXT("List the properties a pipeline class exposes, with types and whether each is editable. "
			 "Call this before queueing a wave of pipeline writes so bad property names are caught "
			 "at planning time rather than after occupying a job slot."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("pipeline_path"), FSololmcpSchemaBuilder::String(
					TEXT("Pipeline asset to take the class from."))},
				{TEXT("pipeline_class"), FSololmcpSchemaBuilder::String(
					TEXT("Class path, used when no pipeline_path is given."))},
				{TEXT("editable_only"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Return only writable properties.")), true)}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_PIPELINES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_pipeline_catalog"));
#else
			const UClass* Class = nullptr;

			FString PipelinePath;
			if (Args->TryGetStringField(TEXT("pipeline_path"), PipelinePath) && !PipelinePath.IsEmpty())
			{
				if (const UObject* Loaded = FSoftObjectPath(PipelinePath).TryLoad())
				{
					Class = Loaded->GetClass();
				}
			}
			if (Class == nullptr)
			{
				FString ClassPath;
				if (!Args->TryGetStringField(TEXT("pipeline_class"), ClassPath) || ClassPath.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("pipeline_class"));
					OutError = TEXT("Pass pipeline_path or pipeline_class.");
					return false;
				}
				Class = Cast<UClass>(FSoftObjectPath(ClassPath).TryLoad());
				if (Class == nullptr)
				{
					SololmcpError::NotFound(OutStructured, ClassPath);
					OutError = FString::Printf(TEXT("Could not resolve '%s'."), *ClassPath);
					return false;
				}
			}

			bool bEditableOnly = true;
			Args->TryGetBoolField(TEXT("editable_only"), bEditableOnly);

			TArray<TSharedPtr<FJsonValue>> AllRows;
			int32 EditableCount = 0;
			BuildPropertyCatalog(Class, AllRows, EditableCount);

			TArray<TSharedPtr<FJsonValue>> Rows;
			if (bEditableOnly)
			{
				for (const TSharedPtr<FJsonValue>& Row : AllRows)
				{
					const TSharedPtr<FJsonObject>* Object = nullptr;
					bool bEditable = false;
					if (Row.IsValid() && Row->TryGetObject(Object) && Object != nullptr
						&& (*Object)->TryGetBoolField(TEXT("editable"), bEditable) && bEditable)
					{
						Rows.Add(Row);
					}
				}
			}
			else
			{
				Rows = AllRows;
			}

			OutStructured->SetStringField(TEXT("pipeline_class"), Class->GetPathName());
			OutStructured->SetArrayField(TEXT("properties"), Rows);
			OutStructured->SetNumberField(TEXT("property_count"), Rows.Num());
			OutStructured->SetNumberField(TEXT("editable_count"), EditableCount);
			OutStructured->SetNumberField(TEXT("total_property_count"), AllRows.Num());
			OutStructured->SetStringField(TEXT("engine_version"), EngineVersionString());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%s exposes %d propert(ies), %d editable."),
				*Class->GetName(), AllRows.Num(), EditableCount);
			return true;
#endif
		},
		nullptr,
		120
	});

	// ── interchange_pipeline_property_set ──────────────────────────────────
	Registry.Register({
		TEXT("interchange_pipeline_property_set"),
		TEXT("Set properties on a pipeline asset and return post-set readback receipts. "
			 "For more than a couple of pipelines use interchange_pipeline_property_set_batch."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("pipeline_path"), FSololmcpSchemaBuilder::String(
					TEXT("Object path of the pipeline asset."))},
				{TEXT("properties"), FSololmcpSchemaBuilder::Object(
					{}, {}, TEXT("Property name to value. Nested paths such as CommonMeshesProperties.bImportLods are supported."))}
			},
			{TEXT("pipeline_path"), TEXT("properties")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_PIPELINES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_pipeline_property_set"));
#else
			FString PipelinePath;
			Args->TryGetStringField(TEXT("pipeline_path"), PipelinePath);
			UInterchangePipelineBase* Pipeline = LoadPipeline(PipelinePath, OutStructured, OutError);
			if (Pipeline == nullptr)
			{
				return false;
			}

			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Args->TryGetObjectField(TEXT("properties"), Properties) || Properties == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("properties"));
				OutError = TEXT("Missing properties object.");
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> Receipts;
			FString ApplyError;
			const bool bApplied = Context.Services.ApplyPropertiesWithReceipts(
				Pipeline, *Properties, Receipts, ApplyError);

			OutStructured->SetStringField(TEXT("pipeline_path"), Pipeline->GetPathName());
			OutStructured->SetArrayField(TEXT("receipts"), Receipts);
			OutStructured->SetNumberField(TEXT("property_count"), (*Properties)->Values.Num());
			OutStructured->SetBoolField(TEXT("ok"), bApplied);
			if (!bApplied)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("properties"),
					TEXT("Run interchange_pipeline_catalog to confirm the property names and types."));
				OutStructured->SetStringField(TEXT("apply_error"), ApplyError);
				OutError = ApplyError.IsEmpty()
					? FString::Printf(TEXT("Property apply failed on '%s'."), *PipelinePath)
					: ApplyError;
				return false;
			}
			OutSummary = FString::Printf(TEXT("Applied %d propert(ies) to pipeline '%s'."),
				(*Properties)->Values.Num(), *Pipeline->GetName());
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_pipeline_property_set_batch ────────────────────────────
	Registry.Register({
		TEXT("interchange_pipeline_property_set_batch"),
		TEXT("Apply property writes across many pipelines in ONE game-thread entry. This is the "
			 "throughput tool for queued reconfiguration: N single writes cost N entries against a "
			 "small concurrent job budget, while this costs one. Per-item receipts are returned."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("items"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object(
						{
							{TEXT("pipeline_path"), FSololmcpSchemaBuilder::String(TEXT("Pipeline asset path."))},
							{TEXT("properties"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Property name to value."))}
						},
						{TEXT("pipeline_path"), TEXT("properties")}),
					TEXT("Pipeline writes to apply, in order."))},
				{TEXT("stop_on_error"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Abort remaining items after the first failure. Defaults to false.")),
					false)}
			},
			{TEXT("items")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_PIPELINES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_pipeline_property_set_batch"));
#else
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Args->TryGetArrayField(TEXT("items"), Items) || Items == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("items"));
				OutError = TEXT("Missing items array.");
				return false;
			}
			bool bStopOnError = false;
			Args->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

			TArray<TSharedPtr<FJsonValue>> Results;
			int32 Succeeded = 0;
			int32 Failed = 0;
			int32 Skipped = 0;

			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("index"), Index);

				if (bStopOnError && Failed > 0)
				{
					Row->SetStringField(TEXT("status"), TEXT("skipped"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Skipped;
					continue;
				}

				const TSharedPtr<FJsonObject>* Item = nullptr;
				if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Item) || Item == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("item_not_object"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				FString PipelinePath;
				(*Item)->TryGetStringField(TEXT("pipeline_path"), PipelinePath);
				Row->SetStringField(TEXT("pipeline_path"), PipelinePath);

				const TSharedPtr<FJsonObject>* Properties = nullptr;
				if (PipelinePath.IsEmpty()
					|| !(*Item)->TryGetObjectField(TEXT("properties"), Properties) || Properties == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("missing_pipeline_path_or_properties"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				UInterchangePipelineBase* Pipeline =
					Cast<UInterchangePipelineBase>(FSoftObjectPath(PipelinePath).TryLoad());
				if (Pipeline == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("pipeline_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				TArray<TSharedPtr<FJsonValue>> Receipts;
				FString ApplyError;
				if (Context.Services.ApplyPropertiesWithReceipts(Pipeline, *Properties, Receipts, ApplyError))
				{
					Row->SetStringField(TEXT("status"), TEXT("ok"));
					Row->SetArrayField(TEXT("receipts"), Receipts);
					++Succeeded;
				}
				else
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), ApplyError.IsEmpty() ? TEXT("apply_failed") : ApplyError);
					++Failed;
				}
				Results.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("results"), Results);
			OutStructured->SetNumberField(TEXT("requested"), Items->Num());
			OutStructured->SetNumberField(TEXT("succeeded"), Succeeded);
			OutStructured->SetNumberField(TEXT("failed"), Failed);
			OutStructured->SetNumberField(TEXT("skipped"), Skipped);
			OutStructured->SetNumberField(TEXT("game_thread_entries"), 1);
			OutStructured->SetBoolField(TEXT("ok"), Failed == 0);
			OutSummary = FString::Printf(
				TEXT("Configured %d/%d pipeline(s) in one game-thread entry (%d failed, %d skipped)."),
				Succeeded, Items->Num(), Failed, Skipped);
			if (Failed > 0)
			{
				OutError = FString::Printf(TEXT("%d of %d pipeline writes failed."), Failed, Items->Num());
				return false;
			}
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_pipeline_class_list ────────────────────────────────────
	Registry.Register({
		TEXT("interchange_pipeline_class_list"),
		TEXT("List the pipeline classes this editor has loaded, so a caller can pick a base class "
			 "for a new pipeline asset without guessing at class paths."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("include_abstract"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Include abstract base classes, which cannot be instantiated.")),
					false)}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_PIPELINES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_pipeline_class_list"));
#else
			bool bIncludeAbstract = false;
			Args->TryGetBoolField(TEXT("include_abstract"), bIncludeAbstract);

			TArray<TSharedPtr<FJsonValue>> Rows;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (!Class->IsChildOf(UInterchangePipelineBase::StaticClass()))
				{
					continue;
				}
				const bool bAbstract = Class->HasAnyClassFlags(CLASS_Abstract);
				if (bAbstract && !bIncludeAbstract)
				{
					continue;
				}
				// Skip the reinstanced/trashed classes hot reload leaves behind.
				if (Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_"))
					|| Class->HasAnyClassFlags(CLASS_NewerVersionExists))
				{
					continue;
				}

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("class_path"), Class->GetPathName());
				Row->SetStringField(TEXT("class_name"), Class->GetName());
				Row->SetBoolField(TEXT("abstract"), bAbstract);
				Row->SetStringField(TEXT("super_class"),
					Class->GetSuperClass() != nullptr ? Class->GetSuperClass()->GetName() : FString());
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("pipeline_classes"), Rows);
			OutStructured->SetNumberField(TEXT("class_count"), Rows.Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d Interchange pipeline class(es) loaded."), Rows.Num());
			return true;
#endif
		},
		nullptr,
		120
	});

	// ── interchange_pipeline_duplicate ─────────────────────────────────────
	Registry.Register({
		TEXT("interchange_pipeline_duplicate"),
		TEXT("Duplicate a pipeline asset so it can be customized without editing the shared default. "
			 "Returns the new asset path, ready for interchange_pipeline_property_set."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("source_pipeline_path"), FSololmcpSchemaBuilder::String(
					TEXT("Pipeline asset to copy."))},
				{TEXT("destination_path"), FSololmcpSchemaBuilder::String(
					TEXT("Destination asset path, e.g. /Game/Pipelines/MyAssetsPipeline."))}
			},
			{TEXT("source_pipeline_path"), TEXT("destination_path")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_PIPELINES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_pipeline_duplicate"));
#else
			FString SourcePath;
			FString DestinationPath;
			Args->TryGetStringField(TEXT("source_pipeline_path"), SourcePath);
			Args->TryGetStringField(TEXT("destination_path"), DestinationPath);
			if (SourcePath.IsEmpty() || DestinationPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured,
					SourcePath.IsEmpty() ? TEXT("source_pipeline_path") : TEXT("destination_path"));
				OutError = TEXT("source_pipeline_path and destination_path are required.");
				return false;
			}
			if (LoadPipeline(SourcePath, OutStructured, OutError) == nullptr)
			{
				return false;
			}

			FString DuplicateError;
			UObject* Duplicated = Context.Services.DuplicateAsset(SourcePath, DestinationPath, DuplicateError);
			if (Duplicated == nullptr)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("destination_path"),
					DuplicateError.IsEmpty()
						? TEXT("Asset duplication returned no object.")
						: DuplicateError);
				OutError = DuplicateError.IsEmpty()
					? FString::Printf(TEXT("Could not duplicate '%s'."), *SourcePath)
					: DuplicateError;
				return false;
			}

			FString SaveError;
			const bool bSaved = Context.Services.SaveAsset(DestinationPath, /*bOnlyIfDirty=*/false, SaveError);

			OutStructured->SetStringField(TEXT("source_pipeline_path"), SourcePath);
			OutStructured->SetStringField(TEXT("pipeline_path"), Duplicated->GetPathName());
			OutStructured->SetBoolField(TEXT("saved"), bSaved);
			if (!bSaved && !SaveError.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("save_error"), SaveError);
			}
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Duplicated pipeline to '%s'%s."),
				*Duplicated->GetPathName(), bSaved ? TEXT(" and saved it") : TEXT(" (unsaved)"));
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_pipeline_stack_set ─────────────────────────────────────
	Registry.Register({
		TEXT("interchange_pipeline_stack_set"),
		TEXT("Replace the pipeline list of a named stack in Project Settings, creating the stack if "
			 "it does not exist. This changes how every subsequent import behaves, so it is a "
			 "project-wide edit rather than a per-import override — use import override_pipelines "
			 "when only one import should differ."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("stack_name"), FSololmcpSchemaBuilder::String(TEXT("Stack to write."))},
				{TEXT("pipelines"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::String(TEXT("Pipeline asset object path.")),
					TEXT("Ordered pipeline list; they execute top to bottom."))},
				{TEXT("scene_import"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Target the scene-import settings.")), false)},
				{TEXT("persist"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Write the change to the project config file. When false the edit only "
							 "lasts for this editor session.")),
					true)}
			},
			{TEXT("stack_name"), TEXT("pipelines")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_PIPELINES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_pipeline_stack_set"));
#else
			FString StackName;
			if (!Args->TryGetStringField(TEXT("stack_name"), StackName) || StackName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("stack_name"));
				OutError = TEXT("Missing stack_name.");
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Pipelines = nullptr;
			if (!Args->TryGetArrayField(TEXT("pipelines"), Pipelines) || Pipelines == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("pipelines"));
				OutError = TEXT("Missing pipelines array.");
				return false;
			}

			// Validate every entry before mutating anything: a half-written stack is
			// worse than a rejected one, because imports would silently use it.
			TArray<FSoftObjectPath> Resolved;
			TArray<FString> Unresolved;
			for (const TSharedPtr<FJsonValue>& Value : *Pipelines)
			{
				FString Path;
				if (!Value.IsValid() || !Value->TryGetString(Path) || Path.IsEmpty())
				{
					continue;
				}
				if (Cast<UInterchangePipelineBase>(FSoftObjectPath(Path).TryLoad()) == nullptr)
				{
					Unresolved.Add(Path);
					continue;
				}
				Resolved.Add(FSoftObjectPath(Path));
			}
			if (Unresolved.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Rows;
				for (const FString& Path : Unresolved)
				{
					Rows.Add(MakeShared<FJsonValueString>(Path));
				}
				OutStructured->SetArrayField(TEXT("unresolved_pipelines"), Rows);
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("pipelines"),
					TEXT("Every entry must load as a UInterchangePipelineBase. Nothing was changed."));
				OutError = FString::Printf(TEXT("%d pipeline path(s) did not resolve."), Unresolved.Num());
				return false;
			}

			bool bSceneImport = false;
			Args->TryGetBoolField(TEXT("scene_import"), bSceneImport);
			bool bPersist = true;
			Args->TryGetBoolField(TEXT("persist"), bPersist);

			FInterchangeImportSettings& Settings =
				FInterchangeProjectSettingsUtils::GetMutableDefaultImportSettings(bSceneImport);
			const bool bCreated = !Settings.PipelineStacks.Contains(FName(*StackName));
			FInterchangePipelineStack& Stack = Settings.PipelineStacks.FindOrAdd(FName(*StackName));
			const int32 PreviousCount = Stack.Pipelines.Num();
			Stack.Pipelines = Resolved;

			bool bPersisted = false;
			if (bPersist)
			{
				UInterchangeProjectSettings* ProjectSettings = GetMutableDefault<UInterchangeProjectSettings>();
				if (ProjectSettings != nullptr)
				{
					bPersisted = ProjectSettings->TryUpdateDefaultConfigFile();
				}
			}

			OutStructured->SetStringField(TEXT("stack_name"), StackName);
			OutStructured->SetBoolField(TEXT("created"), bCreated);
			OutStructured->SetNumberField(TEXT("previous_pipeline_count"), PreviousCount);
			OutStructured->SetNumberField(TEXT("pipeline_count"), Resolved.Num());
			OutStructured->SetBoolField(TEXT("scene_import"), bSceneImport);
			OutStructured->SetBoolField(TEXT("persisted"), bPersisted);
			if (bPersist && !bPersisted)
			{
				OutStructured->SetStringField(TEXT("persist_note"),
					TEXT("The in-memory settings were updated but the config file could not be written; "
						 "the change will not survive an editor restart."));
			}
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%s stack '%s' with %d pipeline(s)%s."),
				bCreated ? TEXT("Created") : TEXT("Updated"), *StackName, Resolved.Num(),
				bPersisted ? TEXT(" and persisted it") : TEXT(" (session only)"));
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_pipeline_stack_default_set ─────────────────────────────
	Registry.Register({
		TEXT("interchange_pipeline_stack_default_set"),
		TEXT("Choose which pipeline stack imports use by default. Affects every subsequent import "
			 "that does not pass override_pipelines."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("stack_name"), FSololmcpSchemaBuilder::String(TEXT("Stack to make default."))},
				{TEXT("scene_import"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Target the scene-import settings.")), false)},
				{TEXT("persist"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Write to the project config file.")), true)}
			},
			{TEXT("stack_name")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_PIPELINES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_pipeline_stack_default_set"));
#else
			FString StackName;
			if (!Args->TryGetStringField(TEXT("stack_name"), StackName) || StackName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("stack_name"));
				OutError = TEXT("Missing stack_name.");
				return false;
			}
			bool bSceneImport = false;
			Args->TryGetBoolField(TEXT("scene_import"), bSceneImport);
			bool bPersist = true;
			Args->TryGetBoolField(TEXT("persist"), bPersist);

			FInterchangeImportSettings& Settings =
				FInterchangeProjectSettingsUtils::GetMutableDefaultImportSettings(bSceneImport);
			if (!Settings.PipelineStacks.Contains(FName(*StackName)))
			{
				TArray<FString> Available;
				for (const TPair<FName, FInterchangePipelineStack>& Pair : Settings.PipelineStacks)
				{
					Available.Add(Pair.Key.ToString());
				}
				TArray<TSharedPtr<FJsonValue>> Rows;
				for (const FString& Name : Available)
				{
					Rows.Add(MakeShared<FJsonValueString>(Name));
				}
				OutStructured->SetArrayField(TEXT("available_stacks"), Rows);
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("stack_name"),
					TEXT("Create the stack with interchange_pipeline_stack_set first."));
				OutError = FString::Printf(TEXT("No pipeline stack named '%s'."), *StackName);
				return false;
			}

			const FName Previous = Settings.DefaultPipelineStack;
			Settings.DefaultPipelineStack = FName(*StackName);

			bool bPersisted = false;
			if (bPersist)
			{
				UInterchangeProjectSettings* ProjectSettings = GetMutableDefault<UInterchangeProjectSettings>();
				if (ProjectSettings != nullptr)
				{
					bPersisted = ProjectSettings->TryUpdateDefaultConfigFile();
				}
			}

			OutStructured->SetStringField(TEXT("stack_name"), StackName);
			OutStructured->SetStringField(TEXT("previous_default"), Previous.ToString());
			OutStructured->SetBoolField(TEXT("scene_import"), bSceneImport);
			OutStructured->SetBoolField(TEXT("persisted"), bPersisted);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Default %s-import stack is now '%s' (was '%s')."),
				bSceneImport ? TEXT("scene") : TEXT("asset"), *StackName, *Previous.ToString());
			return true;
#endif
		},
		nullptr,
		0
	});
}

} // namespace UE::SOMOLMCP
