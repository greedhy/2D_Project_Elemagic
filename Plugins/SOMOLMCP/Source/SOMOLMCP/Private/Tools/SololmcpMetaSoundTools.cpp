// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP — MetaSound starter tools (P1-7)
//
// Tools registered (all metasound_* prefix):
//   metasound_create             — Create a UMetaSoundSource or UMetaSoundPatch asset.
//   metasound_inspect            — Reflect basic metadata: type, IO counts, node-count if reachable.
//   metasound_set_input_default  — Mutate graph input defaults with typed literals.
//   metasound_compile            — Build, register, save, and verify the asset.
//
// The module links MetaSound Engine/Frontend/Editor directly. Creation retains class lookup so
// Source and Patch share one guarded path, while graph mutation and compile use the typed builder
// APIs and require save/readback evidence before returning success.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"
#include "Misc/EngineVersionComparison.h"

#if UE_VERSION_OLDER_THAN(5, 6, 0)

namespace UE::SOMOLMCP
{
void RegisterMetaSoundTools(FSololmcpToolRegistry& Registry)
{
	(void)Registry;
}
} // namespace UE::SOMOLMCP

#else

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendLiteral.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Sound/SoundBase.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace UE::SOMOLMCP
{

// ============================================================================
// Helpers
// ============================================================================

namespace MetaSoundToolsImpl
{
	/** Find an engine UClass by short name without needing the owning header.
	 *  Searches first by name, then falls back to scanning UClass instances. */
	static UClass* FindClassByShortName(const TCHAR* ShortName)
	{
		if (!ShortName || !*ShortName) return nullptr;

		// Fast paths first.
		if (UClass* Direct = FindFirstObject<UClass>(ShortName, EFindFirstObjectOptions::ExactClass))
		{
			return Direct;
		}
		// Fallback scan (slow but safe).
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ShortName)
			{
				return *It;
			}
		}
		return nullptr;
	}

	static UClass* FindMetaSoundSourceClass()  { return FindClassByShortName(TEXT("MetaSoundSource")); }
	static UClass* FindMetaSoundPatchClass()   { return FindClassByShortName(TEXT("MetaSoundPatch"));  }

	/** Resolve an asset path → object, plus determine the MetaSound flavor. */
	enum class EMetaSoundKind : uint8 { Unknown, Source, Patch };

	static UObject* LoadMetaSound(const FSololmcpToolExecutionContext& Context, const FString& AssetPath,
		EMetaSoundKind& OutKind, FString& OutError)
	{
		OutKind = EMetaSoundKind::Unknown;
		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("Missing asset_path.");
			return nullptr;
		}
		UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
		if (!Asset) return nullptr;

		const UClass* SourceCls = FindMetaSoundSourceClass();
		const UClass* PatchCls  = FindMetaSoundPatchClass();
		if (SourceCls && Asset->IsA(SourceCls)) OutKind = EMetaSoundKind::Source;
		else if (PatchCls && Asset->IsA(PatchCls)) OutKind = EMetaSoundKind::Patch;
		else
		{
			OutError = FString::Printf(TEXT("Asset '%s' is not a MetaSoundSource or MetaSoundPatch."), *AssetPath);
			return nullptr;
		}
		return Asset;
	}

	static const TCHAR* KindString(EMetaSoundKind K)
	{
		switch (K)
		{
			case EMetaSoundKind::Source: return TEXT("MetaSoundSource");
			case EMetaSoundKind::Patch:  return TEXT("MetaSoundPatch");
			default:                     return TEXT("unknown");
		}
	}

	static UMetaSoundBuilderBase* FindOrBeginBuilder(UObject* Asset, FString& OutError)
	{
		if (!Asset || !Asset->GetClass()->ImplementsInterface(UMetaSoundDocumentInterface::StaticClass()))
		{
			OutError = TEXT("Asset does not implement IMetaSoundDocumentInterface.");
			return nullptr;
		}
		TScriptInterface<IMetaSoundDocumentInterface> Document;
		Document.SetObject(Asset);
		Document.SetInterface(Cast<IMetaSoundDocumentInterface>(Asset));
		EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
		UMetaSoundBuilderBase* Builder = UMetaSoundEditorSubsystem::GetChecked().FindOrBeginBuilding(Document, Result);
		if (!Builder || Result != EMetaSoundBuilderResult::Succeeded)
		{
			OutError = TEXT("MetaSound editor subsystem could not create or recover a builder for the asset.");
			return nullptr;
		}
		return Builder;
	}

	static bool SaveAndReadBack(UObject* Asset, FString& OutError)
	{
		SololmcpWriteFlush::EnsureFlushed(Asset);
		const FString ObjectPath = Asset ? Asset->GetPathName() : FString();
		if (ObjectPath.IsEmpty() || !UEditorAssetLibrary::SaveAsset(ObjectPath, false))
		{
			OutError = FString::Printf(TEXT("Failed to save MetaSound asset '%s'."), *ObjectPath);
			return false;
		}
		if (!UEditorAssetLibrary::DoesAssetExist(ObjectPath) || !StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath))
		{
			OutError = FString::Printf(TEXT("MetaSound readback failed after saving '%s'."), *ObjectPath);
			return false;
		}
		return true;
	}

	static bool PersistBuilder(UMetaSoundBuilderBase* Builder, UObject* Asset, FString& OutError)
	{
		if (!Builder || !Asset)
		{
			OutError = TEXT("MetaSound builder or asset is null.");
			return false;
		}
		TScriptInterface<IMetaSoundDocumentInterface> Document;
		Document.SetObject(Asset);
		Document.SetInterface(Cast<IMetaSoundDocumentInterface>(Asset));
		Builder->InitNodeLocations();
		Builder->BuildAndOverwriteMetaSound(Document, false);
		UMetaSoundEditorSubsystem::GetChecked().RegisterGraphWithFrontend(*Asset, true);
		return SaveAndReadBack(Asset, OutError);
	}

	static bool ParseNodeId(const FString& Text, FMetaSoundNodeHandle& OutHandle, FString& OutError)
	{
		FGuid NodeId;
		if (!FGuid::Parse(Text, NodeId) || !NodeId.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid MetaSound node_id '%s'."), *Text);
			return false;
		}
		OutHandle = FMetaSoundNodeHandle(NodeId);
		return true;
	}

	static const FMetasoundFrontendNode* FindDocumentNode(const UObject* Asset, const FGuid& NodeId)
	{
		const IMetaSoundDocumentInterface* DocumentInterface = Cast<IMetaSoundDocumentInterface>(Asset);
		if (!DocumentInterface) return nullptr;
		for (const FMetasoundFrontendGraph& Graph : DocumentInterface->GetConstDocument().RootGraph.GetConstGraphPages())
		{
			if (const FMetasoundFrontendNode* Node = Graph.Nodes.FindByPredicate(
				[&NodeId](const FMetasoundFrontendNode& Candidate) { return Candidate.GetID() == NodeId; }))
			{
				return Node;
			}
		}
		return nullptr;
	}

	static TSharedRef<FJsonObject> AnyJsonValueSchema(const FString& Description)
	{
		TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("description"), Description);
		return Schema;
	}

	static bool MakeLiteral(const TSharedRef<FJsonObject>& Arguments, const FName DataType,
		FMetasoundFrontendLiteral& OutLiteral, FString& OutValueText, FString& OutError)
	{
		const TSharedPtr<FJsonValue>* ValuePtr = Arguments->Values.Find(TEXT("value"));
		if (!ValuePtr || !ValuePtr->IsValid())
		{
			OutError = TEXT("Missing value.");
			return false;
		}

		const FString Type = DataType.ToString().ToLower();
		UMetaSoundBuilderSubsystem& Subsystem = UMetaSoundBuilderSubsystem::GetChecked();
		FName LiteralDataType;
		if (Type.Contains(TEXT("bool")))
		{
			bool Value = false;
			if ((*ValuePtr)->Type == EJson::Boolean)
			{
				Value = (*ValuePtr)->AsBool();
			}
			else
			{
				const FString TextValue = (*ValuePtr)->AsString();
				if (!TextValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) && !TextValue.Equals(TEXT("false"), ESearchCase::IgnoreCase))
				{
					OutError = TEXT("Boolean MetaSound input requires true or false.");
					return false;
				}
				Value = TextValue.Equals(TEXT("true"), ESearchCase::IgnoreCase);
			}
			OutLiteral = Subsystem.CreateBoolMetaSoundLiteral(Value, LiteralDataType);
			OutValueText = Value ? TEXT("true") : TEXT("false");
		}
		else if (Type.Contains(TEXT("int")))
		{
			double Number = 0.0;
			if ((*ValuePtr)->Type == EJson::Number) Number = (*ValuePtr)->AsNumber();
			else if (!LexTryParseString(Number, *(*ValuePtr)->AsString()))
			{
				OutError = TEXT("Integer MetaSound input requires an integer value.");
				return false;
			}
			const int32 Value = FMath::RoundToInt(Number);
			OutLiteral = Subsystem.CreateIntMetaSoundLiteral(Value, LiteralDataType);
			OutValueText = LexToString(Value);
		}
		else if (Type.Contains(TEXT("float")) || Type.Contains(TEXT("double")))
		{
			double Number = 0.0;
			if ((*ValuePtr)->Type == EJson::Number) Number = (*ValuePtr)->AsNumber();
			else if (!LexTryParseString(Number, *(*ValuePtr)->AsString()))
			{
				OutError = TEXT("Float MetaSound input requires a numeric value.");
				return false;
			}
			OutLiteral = Subsystem.CreateFloatMetaSoundLiteral(static_cast<float>(Number), LiteralDataType);
			OutValueText = LexToString(Number);
		}
		else if (Type.Contains(TEXT("string")))
		{
			const FString Value = (*ValuePtr)->AsString();
			OutLiteral = Subsystem.CreateStringMetaSoundLiteral(Value, LiteralDataType);
			OutValueText = Value;
		}
		else
		{
			OutError = FString::Printf(TEXT("Unsupported MetaSound graph input type '%s'. Supported literal types are Bool, Int32, Float, and String."), *DataType.ToString());
			return false;
		}
		return true;
	}
}

// ============================================================================
// Tool: metasound_create
// ============================================================================

static void RegisterMetaSoundCreate(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("metasound_create"),
		TEXT("Create a MetaSound Patch or a compiled mono Source starter graph. Templates: empty patch, oneshot/loop/music sine-source graphs with native Builder nodes and save/readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset path, e.g. /Game/Audio/MS_Hit"))},
			{TEXT("template"),   FSololmcpSchemaBuilder::String(TEXT("Starter template (default 'empty')"),
				{TEXT("oneshot"), TEXT("loop"), TEXT("music"), TEXT("empty")})}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}

			FString TemplateStr = TEXT("empty");
			Arguments->TryGetStringField(TEXT("template"), TemplateStr);
			TemplateStr = TemplateStr.ToLower();
			if (TemplateStr != TEXT("empty") && TemplateStr != TEXT("oneshot") &&
				TemplateStr != TEXT("loop") && TemplateStr != TEXT("music"))
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_PARAMETER"), TEXT("template"),
					TEXT("template must be empty, oneshot, loop, or music."));
				OutError = FString::Printf(TEXT("Unsupported MetaSound template '%s'."), *TemplateStr);
				return false;
			}

			// Decide which class to spawn. 'empty' => Patch (reusable graph), all others => Source.
			UClass* TargetClass = nullptr;
			MetaSoundToolsImpl::EMetaSoundKind Kind = MetaSoundToolsImpl::EMetaSoundKind::Unknown;
			if (TemplateStr == TEXT("empty"))
			{
				TargetClass = MetaSoundToolsImpl::FindMetaSoundPatchClass();
				Kind = MetaSoundToolsImpl::EMetaSoundKind::Patch;
				if (!TargetClass)
				{
					// Patch class not registered → fall back to Source.
					TargetClass = MetaSoundToolsImpl::FindMetaSoundSourceClass();
					Kind = MetaSoundToolsImpl::EMetaSoundKind::Source;
				}
			}
			else
			{
				TargetClass = MetaSoundToolsImpl::FindMetaSoundSourceClass();
				Kind = MetaSoundToolsImpl::EMetaSoundKind::Source;
			}

			if (!TargetClass)
			{
				SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"), TEXT(""),
					TEXT("MetaSoundSource/MetaSoundPatch class not registered. Ensure MetasoundEngine + MetasoundFrontend modules are loaded; add to SOMOLMCP.Build.cs to link directly."));
				OutError = TEXT("MetaSound classes not available in this build.");
				return false;
			}

			// Validate path.
			FString PackagePath = AssetPath;
			int32 DotIdx = INDEX_NONE;
			if (PackagePath.FindChar('.', DotIdx)) { PackagePath = PackagePath.Left(DotIdx); }
			if (!FPackageName::IsValidLongPackageName(PackagePath))
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				OutError = FString::Printf(TEXT("Invalid asset_path '%s'."), *AssetPath);
				return false;
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
			if (FPackageName::DoesPackageExist(PackagePath) || UEditorAssetLibrary::DoesAssetExist(PackagePath + TEXT(".") + AssetName))
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("asset_path"),
					FString::Printf(TEXT("MetaSound asset already exists: '%s'."), *PackagePath));
				OutStructured->SetStringField(TEXT("status"), TEXT("no_op"));
				OutError = TEXT("Refusing to overwrite existing MetaSound asset.");
				return false;
			}

			UObject* NewAsset = nullptr;
			bool bBuiltThroughEditorSubsystem = false;
			int32 StarterNodeCount = 0;
			if (TemplateStr != TEXT("empty"))
			{
				EMetaSoundBuilderResult BuildResult = EMetaSoundBuilderResult::Failed;
				FMetaSoundBuilderNodeOutputHandle OnPlayOutput;
				FMetaSoundBuilderNodeInputHandle OnFinishedInput;
				TArray<FMetaSoundBuilderNodeInputHandle> AudioOutputs;
				const bool bOneShot = TemplateStr == TEXT("oneshot");
				const FName BuilderName(*FString::Printf(TEXT("SOMOLMCP_%s_%s"), *AssetName, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
				UMetaSoundSourceBuilder* Builder = UMetaSoundBuilderSubsystem::GetChecked().CreateSourceBuilder(
					BuilderName, OnPlayOutput, OnFinishedInput, AudioOutputs, BuildResult,
					EMetaSoundOutputAudioFormat::Mono, bOneShot);
				if (!Builder || BuildResult != EMetaSoundBuilderResult::Succeeded || AudioOutputs.Num() != 1)
				{
					OutError = TEXT("CreateSourceBuilder failed to produce a mono MetaSound source graph.");
					return false;
				}

				FMetasoundFrontendLiteral FrequencyLiteral;
				FrequencyLiteral.Set(TemplateStr == TEXT("music") ? 261.6256f : 440.0f);
				const FMetaSoundBuilderNodeOutputHandle FrequencyOutput = Builder->AddGraphInputNode(
					TEXT("Frequency"), TEXT("Float"), FrequencyLiteral, BuildResult);
				if (BuildResult != EMetaSoundBuilderResult::Succeeded || !FrequencyOutput.IsSet())
				{
					OutError = TEXT("Failed to create MetaSound Frequency graph input.");
					return false;
				}

				const FMetaSoundNodeHandle Oscillator = Builder->AddNodeByClassName(
					FMetasoundFrontendClassName(TEXT("UE"), TEXT("Sine"), TEXT("Audio")), BuildResult, 1);
				if (BuildResult != EMetaSoundBuilderResult::Succeeded || !Oscillator.IsSet())
				{
					OutError = TEXT("Failed to create MetaSound Sine oscillator node.");
					return false;
				}
				const FMetaSoundBuilderNodeInputHandle OscFrequency = Builder->FindNodeInputByName(Oscillator, TEXT("Frequency"), BuildResult);
				const FMetaSoundBuilderNodeOutputHandle OscAudio = Builder->FindNodeOutputByName(Oscillator, TEXT("Audio"), BuildResult);
				if (!OscFrequency.IsSet() || !OscAudio.IsSet())
				{
					OutError = TEXT("Sine oscillator Frequency/Audio pins were not found.");
					return false;
				}
				Builder->ConnectNodes(FrequencyOutput, OscFrequency, BuildResult);
				if (BuildResult != EMetaSoundBuilderResult::Succeeded)
				{
					OutError = TEXT("Failed to connect Frequency to Sine oscillator.");
					return false;
				}
				Builder->ConnectNodes(OscAudio, AudioOutputs[0], BuildResult);
				if (BuildResult != EMetaSoundBuilderResult::Succeeded)
				{
					OutError = TEXT("Failed to connect Sine oscillator to mono output.");
					return false;
				}
				StarterNodeCount = 2;

				if (bOneShot)
				{
					const FMetaSoundNodeHandle TriggerDelay = Builder->AddNodeByClassName(
						FMetasoundFrontendClassName(TEXT("UE"), TEXT("Trigger Delay"), TEXT("")), BuildResult, 1);
					if (BuildResult != EMetaSoundBuilderResult::Succeeded || !TriggerDelay.IsSet())
					{
						OutError = TEXT("Failed to create Trigger Delay node for oneshot template.");
						return false;
					}
					const FMetaSoundBuilderNodeInputHandle TriggerIn = Builder->FindNodeInputByName(TriggerDelay, TEXT("In"), BuildResult);
					const FMetaSoundBuilderNodeOutputHandle TriggerOut = Builder->FindNodeOutputByName(TriggerDelay, TEXT("Out"), BuildResult);
					if (!TriggerIn.IsSet() || !TriggerOut.IsSet())
					{
						OutError = TEXT("Trigger Delay In/Out pins were not found.");
						return false;
					}
					Builder->ConnectNodes(OnPlayOutput, TriggerIn, BuildResult);
					if (BuildResult == EMetaSoundBuilderResult::Succeeded)
					{
						Builder->ConnectNodes(TriggerOut, OnFinishedInput, BuildResult);
					}
					if (BuildResult != EMetaSoundBuilderResult::Succeeded)
					{
						OutError = TEXT("Failed to connect oneshot OnPlay/OnFinished trigger chain.");
						return false;
					}
					++StarterNodeCount;
				}

				const FString DestinationFolder = FPackageName::GetLongPackagePath(PackagePath);
				TScriptInterface<IMetaSoundDocumentInterface> BuiltDocument =
					UMetaSoundEditorSubsystem::GetChecked().BuildToAsset(
						Builder, TEXT("SOMOLMCP"), AssetName, DestinationFolder, BuildResult);
				NewAsset = BuiltDocument.GetObject();
				if (BuildResult != EMetaSoundBuilderResult::Succeeded || !NewAsset || !NewAsset->IsA(TargetClass))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("asset_path"),
						TEXT("MetaSoundEditorSubsystem::BuildToAsset failed or returned an unexpected asset class."));
					OutError = TEXT("Failed to persist the authored MetaSound graph to an asset.");
					return false;
				}
				bBuiltThroughEditorSubsystem = true;
			}
			else
			{
				UPackage* Package = CreatePackage(*PackagePath);
				if (!Package)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("CreatePackage returned null."));
					OutError = TEXT("Failed to create package.");
					return false;
				}

				NewAsset = NewObject<UObject>(Package, TargetClass, *AssetName, RF_Public | RF_Standalone);
				if (!NewAsset)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
						FString::Printf(TEXT("NewObject<%s> failed."), *TargetClass->GetName()));
					OutError = TEXT("Failed to construct MetaSound asset.");
					return false;
				}
				FAssetRegistryModule::AssetCreated(NewAsset);
			}

			NewAsset->MarkPackageDirty();
			const FString CreatedPath = NewAsset->GetPathName();
			const bool bSaved = UEditorAssetLibrary::SaveAsset(CreatedPath, /*bOnlyIfIsDirty=*/false);
			if (!bSaved)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("asset_path"),
					FString::Printf(TEXT("SaveAsset failed for '%s'."), *CreatedPath));
				OutError = TEXT("Failed to save MetaSound asset.");
				OutSummary = FString::Printf(TEXT("metasound_create failed while saving '%s'."), *CreatedPath);
				return false;
			}
			SololmcpWriteFlush::EnsureFlushed(NewAsset);
			if (!UEditorAssetLibrary::DoesAssetExist(CreatedPath))
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("asset_path"),
					FString::Printf(TEXT("SaveAsset returned true but asset registry cannot find '%s'."), *CreatedPath));
				OutError = TEXT("Saved MetaSound asset was not found by asset registry.");
				return false;
			}

			UObject* ReloadedAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *CreatedPath);
			if (!ReloadedAsset || !ReloadedAsset->IsA(TargetClass))
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("asset_path"),
					FString::Printf(TEXT("Saved asset reload/class validation failed for '%s'."), *CreatedPath));
				OutError = TEXT("Saved MetaSound asset failed reload/class validation.");
				OutSummary = FString::Printf(TEXT("metasound_create failed validation after saving '%s'."), *CreatedPath);
				return false;
			}

			bool bFrequencyInputReadback = TemplateStr == TEXT("empty");
			if (TemplateStr != TEXT("empty"))
			{
				FString BuilderError;
				UMetaSoundBuilderBase* ReadbackBuilder = MetaSoundToolsImpl::FindOrBeginBuilder(ReloadedAsset, BuilderError);
				if (!ReadbackBuilder)
				{
					SololmcpError::Set(OutStructured, TEXT("POST_WRITE_READBACK_FAILED"), TEXT("asset_path"), BuilderError);
					OutError = TEXT("Authored MetaSound could not be reopened for graph readback.");
					return false;
				}
				EMetaSoundBuilderResult ReadbackResult = EMetaSoundBuilderResult::Failed;
				const TArray<FName> ReadbackInputs = ReadbackBuilder->GetGraphInputNames(ReadbackResult);
				bFrequencyInputReadback = ReadbackResult == EMetaSoundBuilderResult::Succeeded
					&& ReadbackInputs.Contains(TEXT("Frequency"));
				if (!bFrequencyInputReadback)
				{
					SololmcpError::Set(OutStructured, TEXT("POST_WRITE_READBACK_FAILED"), TEXT("asset_path"),
						TEXT("The saved MetaSound graph did not retain its Frequency input."));
					OutError = TEXT("MetaSound graph persistence verification failed.");
					return false;
				}
			}

			OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
			OutStructured->SetStringField(TEXT("type"), MetaSoundToolsImpl::KindString(Kind));
			OutStructured->SetStringField(TEXT("template"), TemplateStr);
			OutStructured->SetNumberField(TEXT("starter_node_count"), StarterNodeCount);
			OutStructured->SetBoolField(TEXT("native_builder_template"), TemplateStr != TEXT("empty"));
			OutStructured->SetBoolField(TEXT("built_through_editor_subsystem"), bBuiltThroughEditorSubsystem);
			OutStructured->SetBoolField(TEXT("frequency_input_readback"), bFrequencyInputReadback);
			OutSummary = FString::Printf(TEXT("Created %s '%s' (template=%s)"),
				MetaSoundToolsImpl::KindString(Kind), *CreatedPath, *TemplateStr);
			return true;
		}
	});
}

// ============================================================================
// Tool: metasound_inspect
// ============================================================================

static void RegisterMetaSoundInspect(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("metasound_inspect"),
		TEXT("Return MetaSound type and live graph input/output metadata through the editor builder API."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}

			MetaSoundToolsImpl::EMetaSoundKind Kind = MetaSoundToolsImpl::EMetaSoundKind::Unknown;
			UObject* Asset = MetaSoundToolsImpl::LoadMetaSound(Context, AssetPath, Kind, OutError);
			if (!Asset)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			OutStructured->SetStringField(TEXT("type"), MetaSoundToolsImpl::KindString(Kind));

			UMetaSoundBuilderBase* Builder = MetaSoundToolsImpl::FindOrBeginBuilder(Asset, OutError);
			if (!Builder)
			{
				SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"), TEXT("asset_path"), OutError);
				return false;
			}
			EMetaSoundBuilderResult InputResult = EMetaSoundBuilderResult::Failed;
			EMetaSoundBuilderResult OutputResult = EMetaSoundBuilderResult::Failed;
			const TArray<FName> Inputs = Builder->GetGraphInputNames(InputResult);
			const TArray<FName> Outputs = Builder->GetGraphOutputNames(OutputResult);
			TArray<TSharedPtr<FJsonValue>> InputJson;
			TArray<TSharedPtr<FJsonValue>> OutputJson;
			for (const FName Name : Inputs) InputJson.Add(MakeShared<FJsonValueString>(Name.ToString()));
			for (const FName Name : Outputs) OutputJson.Add(MakeShared<FJsonValueString>(Name.ToString()));
			OutStructured->SetNumberField(TEXT("input_count"), Inputs.Num());
			OutStructured->SetNumberField(TEXT("output_count"), Outputs.Num());
			OutStructured->SetArrayField(TEXT("inputs"), InputJson);
			OutStructured->SetArrayField(TEXT("outputs"), OutputJson);
			OutStructured->SetBoolField(TEXT("builder_ready"), InputResult == EMetaSoundBuilderResult::Succeeded && OutputResult == EMetaSoundBuilderResult::Succeeded);

			const IMetaSoundDocumentInterface* DocumentInterface = Cast<IMetaSoundDocumentInterface>(Asset);
			TArray<TSharedPtr<FJsonValue>> NodesJson;
			int32 EdgeCount = 0;
			if (DocumentInterface)
			{
				const FMetasoundFrontendDocument& Document = DocumentInterface->GetConstDocument();
				for (const FMetasoundFrontendGraph& Graph : Document.RootGraph.GetConstGraphPages())
				{
					EdgeCount += Graph.Edges.Num();
					for (const FMetasoundFrontendNode& Node : Graph.Nodes)
					{
						TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
						TArray<TSharedPtr<FJsonValue>> NodeInputsJson;
						TArray<TSharedPtr<FJsonValue>> NodeOutputsJson;
						for (const FMetasoundFrontendVertex& Input : Node.Interface.Inputs)
						{
							TSharedRef<FJsonObject> Pin = MakeShared<FJsonObject>();
							Pin->SetStringField(TEXT("name"), Input.Name.ToString());
							Pin->SetStringField(TEXT("data_type"), Input.TypeName.ToString());
							Pin->SetStringField(TEXT("vertex_id"), Input.VertexID.ToString(EGuidFormats::DigitsWithHyphensLower));
							NodeInputsJson.Add(MakeShared<FJsonValueObject>(Pin));
						}
						for (const FMetasoundFrontendVertex& Output : Node.Interface.Outputs)
						{
							TSharedRef<FJsonObject> Pin = MakeShared<FJsonObject>();
							Pin->SetStringField(TEXT("name"), Output.Name.ToString());
							Pin->SetStringField(TEXT("data_type"), Output.TypeName.ToString());
							Pin->SetStringField(TEXT("vertex_id"), Output.VertexID.ToString(EGuidFormats::DigitsWithHyphensLower));
							NodeOutputsJson.Add(MakeShared<FJsonValueObject>(Pin));
						}
						NodeJson->SetStringField(TEXT("node_id"), Node.GetID().ToString(EGuidFormats::DigitsWithHyphensLower));
						NodeJson->SetStringField(TEXT("page_id"), Graph.PageID.ToString(EGuidFormats::DigitsWithHyphensLower));
						NodeJson->SetStringField(TEXT("name"), Node.Name.ToString());
						NodeJson->SetStringField(TEXT("class_id"), Node.ClassID.ToString(EGuidFormats::DigitsWithHyphensLower));
						NodeJson->SetNumberField(TEXT("input_count"), Node.Interface.Inputs.Num());
						NodeJson->SetNumberField(TEXT("output_count"), Node.Interface.Outputs.Num());
						NodeJson->SetArrayField(TEXT("inputs"), NodeInputsJson);
						NodeJson->SetArrayField(TEXT("outputs"), NodeOutputsJson);
						NodesJson.Add(MakeShared<FJsonValueObject>(NodeJson));
					}
				}
			}
			OutStructured->SetNumberField(TEXT("node_count"), NodesJson.Num());
			OutStructured->SetNumberField(TEXT("edge_count"), EdgeCount);
			OutStructured->SetArrayField(TEXT("nodes"), NodesJson);

			OutSummary = FString::Printf(TEXT("Inspected MetaSound '%s' (%s): %d inputs, %d outputs, %d nodes, %d edges."),
				*Asset->GetPathName(), MetaSoundToolsImpl::KindString(Kind), Inputs.Num(), Outputs.Num(), NodesJson.Num(), EdgeCount);
			return true;
		}
	});
}

// ============================================================================
// Tool: metasound_set_input_default
// ============================================================================

static void RegisterMetaSoundSetInputDefault(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("metasound_set_input_default"),
		TEXT("Set a Bool, Int32, Float, or String graph input default, overwrite the MetaSound, save it, and verify readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("input_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("value"),      MetaSoundToolsImpl::AnyJsonValueSchema(TEXT("Boolean, integer, number, or string literal."))}
		}, {TEXT("asset_path"), TEXT("input_name"), TEXT("value")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			FString InputName;
			Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
			Arguments->TryGetStringField(TEXT("input_name"), InputName);

			MetaSoundToolsImpl::EMetaSoundKind Kind = MetaSoundToolsImpl::EMetaSoundKind::Unknown;
			UObject* Asset = MetaSoundToolsImpl::LoadMetaSound(Context, AssetPath, Kind, OutError);
			if (!Asset) return false;
			UMetaSoundBuilderBase* Builder = MetaSoundToolsImpl::FindOrBeginBuilder(Asset, OutError);
			if (!Builder) return false;

			EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
			FName DataType;
			FMetaSoundBuilderNodeOutputHandle OutputHandle;
			Builder->FindGraphInputNode(FName(*InputName), DataType, OutputHandle, FindResult);
			if (FindResult != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("MetaSound graph input '%s' does not exist."), *InputName);
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("input_name"), OutError);
				return false;
			}

			FMetasoundFrontendLiteral Literal;
			FString ValueText;
			if (!MetaSoundToolsImpl::MakeLiteral(Arguments, DataType, Literal, ValueText, OutError))
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_PARAMETER"), TEXT("value"), OutError);
				return false;
			}
			EMetaSoundBuilderResult SetResult = EMetaSoundBuilderResult::Failed;
			Builder->SetGraphInputDefault(FName(*InputName), Literal, SetResult);
			if (SetResult != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("SetGraphInputDefault failed for '%s'."), *InputName);
				return false;
			}

			TScriptInterface<IMetaSoundDocumentInterface> Document;
			Document.SetObject(Asset);
			Document.SetInterface(Cast<IMetaSoundDocumentInterface>(Asset));
			Builder->BuildAndOverwriteMetaSound(Document, false);
			UMetaSoundEditorSubsystem::GetChecked().RegisterGraphWithFrontend(*Asset, true);
			if (!MetaSoundToolsImpl::SaveAndReadBack(Asset, OutError)) return false;

			EMetaSoundBuilderResult ReadResult = EMetaSoundBuilderResult::Failed;
			const FMetasoundFrontendLiteral Readback = Builder->GetGraphInputDefault(FName(*InputName), ReadResult);
			if (ReadResult != EMetaSoundBuilderResult::Succeeded || Readback.GetType() != Literal.GetType())
			{
				OutError = TEXT("MetaSound default-value readback did not match the written literal type.");
				return false;
			}
			OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			OutStructured->SetStringField(TEXT("input_name"), InputName);
			OutStructured->SetStringField(TEXT("data_type"), DataType.ToString());
			OutStructured->SetStringField(TEXT("value"), ValueText);
			OutStructured->SetBoolField(TEXT("saved"), true);
			OutStructured->SetBoolField(TEXT("readback_verified"), true);
			OutSummary = FString::Printf(TEXT("Set MetaSound '%s' input '%s' (%s) to '%s'; saved and verified."),
				*Asset->GetPathName(), *InputName, *DataType.ToString(), *ValueText);
			return true;
		}
	});
}

// ============================================================================
// Node authoring tools
// ============================================================================

static void RegisterMetaSoundNodeAuthoring(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("metasound_node_add"),
		TEXT("Add a native MetaSound node by frontend class name, persist the graph, and verify node-ID readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("namespace"), FSololmcpSchemaBuilder::String(TEXT("Frontend class namespace, for example UE"))},
			{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Frontend class name, for example Sine"))},
			{TEXT("variant"), FSololmcpSchemaBuilder::String(TEXT("Frontend class variant, for example Audio"))},
			{TEXT("major_version"), FSololmcpSchemaBuilder::Integer(TEXT("Major node-class version; default 1"))}
		}, {TEXT("asset_path"), TEXT("namespace"), TEXT("name")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath, Namespace, Name, Variant;
			Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
			Arguments->TryGetStringField(TEXT("namespace"), Namespace);
			Arguments->TryGetStringField(TEXT("name"), Name);
			Arguments->TryGetStringField(TEXT("variant"), Variant);
			const int32 MajorVersion = Arguments->HasField(TEXT("major_version")) ? Arguments->GetIntegerField(TEXT("major_version")) : 1;

			MetaSoundToolsImpl::EMetaSoundKind Kind = MetaSoundToolsImpl::EMetaSoundKind::Unknown;
			UObject* Asset = MetaSoundToolsImpl::LoadMetaSound(Context, AssetPath, Kind, OutError);
			if (!Asset) return false;
			UMetaSoundBuilderBase* Builder = MetaSoundToolsImpl::FindOrBeginBuilder(Asset, OutError);
			if (!Builder) return false;

			EMetaSoundBuilderResult AddResult = EMetaSoundBuilderResult::Failed;
			const FMetaSoundNodeHandle Node = Builder->AddNodeByClassName(
				FMetasoundFrontendClassName(FName(*Namespace), FName(*Name), FName(*Variant)), AddResult, MajorVersion);
			if (AddResult != EMetaSoundBuilderResult::Succeeded || !Node.IsSet())
			{
				OutError = FString::Printf(TEXT("MetaSound class '%s.%s.%s' version %d could not be added."),
					*Namespace, *Name, *Variant, MajorVersion);
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("name"), OutError);
				return false;
			}
			if (!MetaSoundToolsImpl::PersistBuilder(Builder, Asset, OutError)) return false;
			const FMetasoundFrontendNode* ReadbackNode = MetaSoundToolsImpl::FindDocumentNode(Asset, Node.NodeID);
			if (!ReadbackNode)
			{
				OutError = TEXT("MetaSound node was not present after save/readback.");
				SololmcpError::Set(OutStructured, TEXT("POST_WRITE_READBACK_FAILED"), TEXT("node_id"), OutError);
				return false;
			}
			OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			OutStructured->SetStringField(TEXT("node_id"), Node.NodeID.ToString(EGuidFormats::DigitsWithHyphensLower));
			OutStructured->SetStringField(TEXT("node_name"), ReadbackNode->Name.ToString());
			OutStructured->SetBoolField(TEXT("saved"), true);
			OutStructured->SetBoolField(TEXT("readback_verified"), true);
			OutSummary = FString::Printf(TEXT("Added and verified MetaSound node %s (%s.%s.%s)."),
				*Node.NodeID.ToString(EGuidFormats::DigitsWithHyphensLower), *Namespace, *Name, *Variant);
			return true;
		}
	});

	Registry.Register({
		TEXT("metasound_node_remove"),
		TEXT("Remove a MetaSound node by persistent node ID, save, and verify the node no longer exists."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("node_id"), FSololmcpSchemaBuilder::String()}
		}, {TEXT("asset_path"), TEXT("node_id")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath, NodeIdText;
			Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
			Arguments->TryGetStringField(TEXT("node_id"), NodeIdText);
			FMetaSoundNodeHandle Node;
			if (!MetaSoundToolsImpl::ParseNodeId(NodeIdText, Node, OutError)) return false;
			MetaSoundToolsImpl::EMetaSoundKind Kind = MetaSoundToolsImpl::EMetaSoundKind::Unknown;
			UObject* Asset = MetaSoundToolsImpl::LoadMetaSound(Context, AssetPath, Kind, OutError);
			if (!Asset) return false;
			UMetaSoundBuilderBase* Builder = MetaSoundToolsImpl::FindOrBeginBuilder(Asset, OutError);
			if (!Builder) return false;
			if (!Builder->ContainsNode(Node))
			{
				OutError = FString::Printf(TEXT("MetaSound node '%s' does not exist."), *NodeIdText);
				return false;
			}
			EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
			Builder->RemoveNode(Node, RemoveResult, true);
			if (RemoveResult != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("Failed to remove MetaSound node '%s'."), *NodeIdText);
				return false;
			}
			if (!MetaSoundToolsImpl::PersistBuilder(Builder, Asset, OutError)) return false;
			if (MetaSoundToolsImpl::FindDocumentNode(Asset, Node.NodeID))
			{
				OutError = TEXT("Removed MetaSound node still exists after save/readback.");
				SololmcpError::Set(OutStructured, TEXT("POST_WRITE_READBACK_FAILED"), TEXT("node_id"), OutError);
				return false;
			}
			OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			OutStructured->SetStringField(TEXT("node_id"), NodeIdText);
			OutStructured->SetBoolField(TEXT("removed"), true);
			OutStructured->SetBoolField(TEXT("readback_verified"), true);
			OutSummary = FString::Printf(TEXT("Removed and verified MetaSound node '%s'."), *NodeIdText);
			return true;
		}
	});

	auto RegisterConnectionTool = [&Registry](const TCHAR* ToolName, bool bConnect)
	{
		Registry.Register({
			ToolName,
			bConnect ? TEXT("Connect named output/input pins on two persistent MetaSound nodes and verify the edge after save.")
				: TEXT("Disconnect named output/input pins on two persistent MetaSound nodes and verify edge removal after save."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("from_node_id"), FSololmcpSchemaBuilder::String()},
				{TEXT("from_output"), FSololmcpSchemaBuilder::String()},
				{TEXT("to_node_id"), FSololmcpSchemaBuilder::String()},
				{TEXT("to_input"), FSololmcpSchemaBuilder::String()}
			}, {TEXT("asset_path"), TEXT("from_node_id"), TEXT("from_output"), TEXT("to_node_id"), TEXT("to_input")}),
			[bConnect](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, FromIdText, FromOutput, ToIdText, ToInput;
				Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
				Arguments->TryGetStringField(TEXT("from_node_id"), FromIdText);
				Arguments->TryGetStringField(TEXT("from_output"), FromOutput);
				Arguments->TryGetStringField(TEXT("to_node_id"), ToIdText);
				Arguments->TryGetStringField(TEXT("to_input"), ToInput);
				FMetaSoundNodeHandle FromNode, ToNode;
				if (!MetaSoundToolsImpl::ParseNodeId(FromIdText, FromNode, OutError)
					|| !MetaSoundToolsImpl::ParseNodeId(ToIdText, ToNode, OutError)) return false;
				MetaSoundToolsImpl::EMetaSoundKind Kind = MetaSoundToolsImpl::EMetaSoundKind::Unknown;
				UObject* Asset = MetaSoundToolsImpl::LoadMetaSound(Context, AssetPath, Kind, OutError);
				if (!Asset) return false;
				UMetaSoundBuilderBase* Builder = MetaSoundToolsImpl::FindOrBeginBuilder(Asset, OutError);
				if (!Builder || !Builder->ContainsNode(FromNode) || !Builder->ContainsNode(ToNode))
				{
					OutError = TEXT("One or both MetaSound node IDs do not exist in the target graph.");
					return false;
				}
				EMetaSoundBuilderResult PinResult = EMetaSoundBuilderResult::Failed;
				const FMetaSoundBuilderNodeOutputHandle Output = Builder->FindNodeOutputByName(FromNode, FName(*FromOutput), PinResult);
				if (PinResult != EMetaSoundBuilderResult::Succeeded || !Output.IsSet())
				{
					OutError = FString::Printf(TEXT("Output pin '%s' was not found."), *FromOutput);
					return false;
				}
				const FMetaSoundBuilderNodeInputHandle Input = Builder->FindNodeInputByName(ToNode, FName(*ToInput), PinResult);
				if (PinResult != EMetaSoundBuilderResult::Succeeded || !Input.IsSet())
				{
					OutError = FString::Printf(TEXT("Input pin '%s' was not found."), *ToInput);
					return false;
				}
				EMetaSoundBuilderResult MutationResult = EMetaSoundBuilderResult::Failed;
				if (bConnect) Builder->ConnectNodes(Output, Input, MutationResult);
				else Builder->DisconnectNodes(Output, Input, MutationResult);
				if (MutationResult != EMetaSoundBuilderResult::Succeeded)
				{
					OutError = bConnect ? TEXT("MetaSound pin connection failed.") : TEXT("MetaSound pin disconnection failed.");
					return false;
				}
				if (!MetaSoundToolsImpl::PersistBuilder(Builder, Asset, OutError)) return false;
				const bool bConnected = Builder->NodesAreConnected(Output, Input);
				if (bConnected != bConnect)
				{
					OutError = TEXT("MetaSound edge readback did not match the requested state.");
					SololmcpError::Set(OutStructured, TEXT("POST_WRITE_READBACK_FAILED"), TEXT(""), OutError);
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName());
				OutStructured->SetBoolField(TEXT("connected"), bConnected);
				OutStructured->SetBoolField(TEXT("saved"), true);
				OutStructured->SetBoolField(TEXT("readback_verified"), true);
				OutSummary = FString::Printf(TEXT("MetaSound edge %s and verified (%s.%s -> %s.%s)."),
					bConnect ? TEXT("connected") : TEXT("disconnected"), *FromIdText, *FromOutput, *ToIdText, *ToInput);
				return true;
			}
		});
	};

	RegisterConnectionTool(TEXT("metasound_nodes_connect"), true);
	RegisterConnectionTool(TEXT("metasound_nodes_disconnect"), false);
}

// ============================================================================
// Tool: metasound_compile
// ============================================================================

static void RegisterMetaSoundCompile(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("metasound_compile"),
		TEXT("Build and overwrite a MetaSound through the editor builder, register it with the frontend, save, and verify readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);

			MetaSoundToolsImpl::EMetaSoundKind Kind = MetaSoundToolsImpl::EMetaSoundKind::Unknown;
			UObject* Asset = MetaSoundToolsImpl::LoadMetaSound(Context, AssetPath, Kind, OutError);
			if (!Asset) return false;
			UMetaSoundBuilderBase* Builder = MetaSoundToolsImpl::FindOrBeginBuilder(Asset, OutError);
			if (!Builder) return false;

			TScriptInterface<IMetaSoundDocumentInterface> Document;
			Document.SetObject(Asset);
			Document.SetInterface(Cast<IMetaSoundDocumentInterface>(Asset));
			Builder->InitNodeLocations();
			Builder->BuildAndOverwriteMetaSound(Document, false);
			UMetaSoundEditorSubsystem::GetChecked().RegisterGraphWithFrontend(*Asset, true);
			if (!MetaSoundToolsImpl::SaveAndReadBack(Asset, OutError)) return false;

			EMetaSoundBuilderResult InputResult = EMetaSoundBuilderResult::Failed;
			EMetaSoundBuilderResult OutputResult = EMetaSoundBuilderResult::Failed;
			const int32 InputCount = Builder->GetGraphInputNames(InputResult).Num();
			const int32 OutputCount = Builder->GetGraphOutputNames(OutputResult).Num();
			const bool bReadback = InputResult == EMetaSoundBuilderResult::Succeeded && OutputResult == EMetaSoundBuilderResult::Succeeded;
			OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			OutStructured->SetBoolField(TEXT("success"), bReadback);
			OutStructured->SetBoolField(TEXT("saved"), true);
			OutStructured->SetBoolField(TEXT("readback_verified"), bReadback);
			OutStructured->SetNumberField(TEXT("input_count"), InputCount);
			OutStructured->SetNumberField(TEXT("output_count"), OutputCount);
			TArray<TSharedPtr<FJsonValue>> EmptyArr;
			OutStructured->SetArrayField(TEXT("errors"),   EmptyArr);
			OutStructured->SetArrayField(TEXT("warnings"), EmptyArr);
			if (!bReadback)
			{
				OutError = TEXT("MetaSound build completed but builder readback failed.");
				return false;
			}
			OutSummary = FString::Printf(TEXT("Built, registered, saved, and verified MetaSound '%s' (%d inputs, %d outputs)."),
				*Asset->GetPathName(), InputCount, OutputCount);
			return true;
		}
	});
}

// ============================================================================
// Registration entry point
// ============================================================================

void RegisterMetaSoundTools(FSololmcpToolRegistry& Registry)
{
	RegisterMetaSoundCreate(Registry);
	RegisterMetaSoundInspect(Registry);
	RegisterMetaSoundSetInputDefault(Registry);
	RegisterMetaSoundNodeAuthoring(Registry);
	RegisterMetaSoundCompile(Registry);
}

} // namespace UE::SOMOLMCP

#endif // UE_VERSION_OLDER_THAN(5, 6, 0)
