// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// Native-only authoring QA/delivery gates and retired Python PCG replacements.

#include "Tools/SololmcpAuthoringQaPcgCompletionTools.h"

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpTransactionScope.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PackageTools.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP
{
namespace AuthoringQaPcgCompletion
{
	static FCriticalSection StateMutex;
	static TMap<FString, TSharedPtr<FJsonObject>> Snapshots;
	static TMap<FString, TSharedPtr<FJsonObject>> RuntimeSmokes;

	struct FRegisteredReceipt
	{
		TSharedPtr<FJsonObject> Receipt;
		FString CanonicalJson;
		FString Tool;
		FString ProjectName;
		FString AssetPath;
		FString TransactionId;
		FDateTime IssuedAt;
	};

	static TMap<FString, FRegisteredReceipt> ReceiptRegistry;
	static constexpr double ReceiptLifetimeSeconds = 24.0 * 60.0 * 60.0;

	static FString UtcNow()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	static FString NewId(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	static TArray<TSharedPtr<FJsonValue>> StringsJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static FString JsonString(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
		FJsonSerializer::Serialize(Object, Writer);
		return Result;
	}

	static bool ReadJsonObjectArgument(
		const TSharedRef<FJsonObject>& Args,
		const TCHAR* ObjectField,
		const TCHAR* JsonField,
		TSharedPtr<FJsonObject>& Value,
		FString& Error)
	{
		const TSharedPtr<FJsonObject>* Supplied = nullptr;
		if (Args->TryGetObjectField(ObjectField, Supplied) && Supplied && Supplied->IsValid())
		{
			Value = *Supplied;
			return true;
		}
		FString Json;
		if (!Args->TryGetStringField(JsonField, Json) || Json.TrimStartAndEnd().IsEmpty()) return true;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Value) || !Value.IsValid())
		{
			Error = FString::Printf(TEXT("%s must contain a JSON object."), JsonField);
			return false;
		}
		return true;
	}

	static FString Sha1Hex(const TArray<uint8>& Bytes)
	{
		FSHAHash Hash;
		FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
		return Hash.ToString();
	}

	static FString Sha1Hex(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		FSHAHash Hash;
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash.Hash);
		return Hash.ToString();
	}

	static FString CanonicalAssetPath(const FString& Path)
	{
		FString Result = Path;
		Result.TrimStartAndEndInline();
		if (!Result.IsEmpty())
		{
			Result = FPackageName::ObjectPathToPackageName(Result);
		}
		return Result;
	}

	static bool ResolvePcgAssetPath(
		const TSharedRef<FJsonObject>& Args,
		FString& AssetPath,
		FString& Error)
	{
		TArray<FString> Candidates;
		auto AddCandidate = [&Candidates](FString Value)
		{
			Value = CanonicalAssetPath(Value);
			if (!Value.IsEmpty()) Candidates.AddUnique(Value);
		};

		FString Value;
		if (Args->TryGetStringField(TEXT("asset_path"), Value)) AddCandidate(Value);
		if (Args->TryGetStringField(TEXT("graph_path"), Value)) AddCandidate(Value);

		FString PackagePath;
		FString AssetName;
		const bool bHasPackagePath = Args->TryGetStringField(TEXT("package_path"), PackagePath) && !PackagePath.TrimStartAndEnd().IsEmpty();
		const bool bHasAssetName = Args->TryGetStringField(TEXT("asset_name"), AssetName) && !AssetName.TrimStartAndEnd().IsEmpty();
		if (bHasPackagePath != bHasAssetName)
		{
			Error = TEXT("package_path and asset_name must be supplied together.");
			return false;
		}
		if (bHasPackagePath)
		{
			PackagePath.TrimStartAndEndInline();
			AssetName.TrimStartAndEndInline();
			PackagePath.RemoveFromEnd(TEXT("/"));
			if (AssetName.Contains(TEXT("/")) || AssetName.Contains(TEXT(".")))
			{
				Error = TEXT("asset_name must be an unqualified UE asset name.");
				return false;
			}
			AddCandidate(PackagePath + TEXT("/") + AssetName);
		}

		if (Candidates.IsEmpty())
		{
			Error = TEXT("Provide asset_path, graph_path, or package_path with asset_name.");
			return false;
		}
		if (Candidates.Num() != 1)
		{
			Error = TEXT("Conflicting PCG graph path aliases were supplied.");
			return false;
		}

		AssetPath = Candidates[0];
		if (!AssetPath.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(AssetPath))
		{
			Error = FString::Printf(TEXT("A valid /Game/... PCG graph path is required: %s"), *AssetPath);
			return false;
		}
		return true;
	}

	static bool ResolvePackageBytes(
		const FString& AssetPath,
		FString& PackageFilename,
		TArray<uint8>& Bytes,
		FString& Fingerprint,
		FString& Error)
	{
		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		if (!FPackageName::DoesPackageExist(PackageName, &PackageFilename) || PackageFilename.IsEmpty())
		{
			Error = FString::Printf(TEXT("Serialized package does not exist for %s."), *PackageName);
			return false;
		}
		if (!FFileHelper::LoadFileToArray(Bytes, *PackageFilename) || Bytes.IsEmpty())
		{
			Error = FString::Printf(TEXT("Serialized package is empty or unreadable: %s"), *PackageFilename);
			return false;
		}
		Fingerprint = Sha1Hex(Bytes);
		return !Fingerprint.IsEmpty();
	}

	static bool ReadProjectBinding(const TSharedRef<FJsonObject>& Args, FString& Requested)
	{
		if (!Args->TryGetStringField(TEXT("target_project_name"), Requested))
		{
			Args->TryGetStringField(TEXT("project_name"), Requested);
		}
		Requested.TrimStartAndEndInline();
		return !Requested.IsEmpty();
	}

	static bool TargetGuard(
		const TSharedRef<FJsonObject>& Args,
		const FString& AssetPath,
		const bool bRequireAsset,
		TSharedRef<FJsonObject>& Receipt,
		FString& Error)
	{
		FString RequestedProject;
		const FString CurrentProject = FApp::GetProjectName();
		Receipt->SetStringField(TEXT("current_project_name"), CurrentProject);
		Receipt->SetStringField(TEXT("target_asset"), AssetPath);
		if (!ReadProjectBinding(Args, RequestedProject))
		{
			Receipt->SetStringField(TEXT("reason_code"), TEXT("blocked_no_target_guard"));
			Error = TEXT("target_project_name is required for authoring and PCG operations.");
			return false;
		}
		Receipt->SetStringField(TEXT("requested_project_name"), RequestedProject);
		if (!RequestedProject.Equals(CurrentProject, ESearchCase::IgnoreCase))
		{
			Receipt->SetStringField(TEXT("reason_code"), TEXT("blocked_target_project_mismatch"));
			Error = FString::Printf(TEXT("Target project '%s' does not match the active editor project '%s'."), *RequestedProject, *CurrentProject);
			return false;
		}
		if (bRequireAsset)
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
			if (AssetPath.IsEmpty() || !PackageName.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(PackageName))
			{
				Receipt->SetStringField(TEXT("reason_code"), TEXT("blocked_invalid_target_asset"));
				Error = TEXT("A valid /Game/... target asset path is required.");
				return false;
			}
		}
		Receipt->SetStringField(TEXT("status"), TEXT("passed"));
		Receipt->SetBoolField(TEXT("passed"), true);
		return true;
	}

	static TSharedRef<FJsonObject> BaseReceipt(const FString& Tool)
	{
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("schema"), TEXT("somolmcp.authoring_qa_native_receipt.v1"));
		Receipt->SetStringField(TEXT("tool"), Tool);
		Receipt->SetStringField(TEXT("receipt_id"), NewId(TEXT("authoring")));
		Receipt->SetStringField(TEXT("timestamp_utc"), UtcNow());
		Receipt->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
		Receipt->SetBoolField(TEXT("python_used"), false);
		return Receipt;
	}

	static TSharedRef<FJsonObject> ObjectSnapshot(UObject* Object)
	{
		TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		if (!Object)
		{
			Snapshot->SetBoolField(TEXT("exists"), false);
			return Snapshot;
		}
		Snapshot->SetBoolField(TEXT("exists"), true);
		Snapshot->SetStringField(TEXT("object_path"), Object->GetPathName());
		Snapshot->SetStringField(TEXT("class_path"), Object->GetClass()->GetPathName());
		Snapshot->SetStringField(TEXT("package_name"), Object->GetOutermost()->GetName());
		Snapshot->SetBoolField(TEXT("package_dirty"), Object->GetOutermost()->IsDirty());
		Snapshot->SetNumberField(TEXT("object_flags"), static_cast<double>(Object->GetFlags()));
		return Snapshot;
	}

	static TSharedRef<FJsonObject> PcgGraphSnapshot(UPCGGraph* Graph)
	{
		TSharedRef<FJsonObject> Snapshot = ObjectSnapshot(Graph);
		if (!Graph)
		{
			return Snapshot;
		}
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (int32 Index = 0; Index < Graph->GetNodes().Num(); ++Index)
		{
			UPCGNode* Node = Graph->GetNodes()[Index];
			if (!Node) continue;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), Index);
			Row->SetStringField(TEXT("name"), Node->GetName());
			Row->SetStringField(TEXT("title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
			Row->SetStringField(TEXT("settings_class"), Node->GetSettings() ? Node->GetSettings()->GetClass()->GetPathName() : FString());
			Row->SetNumberField(TEXT("input_pin_count"), Node->GetInputPins().Num());
			Row->SetNumberField(TEXT("output_pin_count"), Node->GetOutputPins().Num());
			Nodes.Add(MakeShared<FJsonValueObject>(Row));
		}
		Snapshot->SetArrayField(TEXT("nodes"), Nodes);
		Snapshot->SetNumberField(TEXT("node_count"), Nodes.Num());
		Snapshot->SetNumberField(TEXT("edge_count"), Graph->GetAllEdges().Num());
		return Snapshot;
	}

	static FString PcgGraphContentFingerprint(UPCGGraph* Graph)
	{
		if (!Graph) return FString();

		TArray<FString> NodeRows;
		for (UPCGNode* Node : Graph->GetNodes())
		{
			if (!Node) continue;
			TArray<FString> PinRows;
			auto AddPins = [&PinRows](const TArray<TObjectPtr<UPCGPin>>& Pins, const TCHAR* Direction)
			{
				for (const UPCGPin* Pin : Pins)
				{
					if (!Pin) continue;
					TArray<FString> TypeIds;
					for (const FPCGDataTypeBaseId& TypeId : Pin->Properties.AllowedTypes.GetIds()) TypeIds.Add(TypeId.ToString());
					TypeIds.Sort();
					PinRows.Add(FString::Printf(TEXT("%s|%s|%d|%s"), Direction,
						*Pin->Properties.Label.ToString(), Pin->Edges.Num(), *FString::Join(TypeIds, TEXT(","))));
				}
			};
			AddPins(Node->GetInputPins(), TEXT("in"));
			AddPins(Node->GetOutputPins(), TEXT("out"));
			PinRows.Sort();
			NodeRows.Add(FString::Printf(TEXT("%s|%s|%s|%s"), *Node->GetName(),
				*Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString(),
				Node->GetSettings() ? *Node->GetSettings()->GetClass()->GetPathName() : TEXT(""),
				*FString::Join(PinRows, TEXT(";"))));
		}
		NodeRows.Sort();
		return Sha1Hex(FString::Printf(TEXT("nodes=%d|edges=%d|%s"), Graph->GetNodes().Num(),
			Graph->GetAllEdges().Num(), *FString::Join(NodeRows, TEXT("\n"))));
	}

	static UPCGGraph* LoadPcgGraph(
		const FSololmcpToolExecutionContext& Context,
		const FString& AssetPath,
		FString& Error)
	{
		UObject* Asset = Context.Services.LoadAsset(AssetPath, Error);
		UPCGGraphInterface* Interface = Cast<UPCGGraphInterface>(Asset);
		UPCGGraph* Graph = Interface ? Interface->GetMutablePCGGraph() : Cast<UPCGGraph>(Asset);
		if (!Graph)
		{
			Error = FString::Printf(TEXT("Asset is not a mutable PCG graph: %s"), *AssetPath);
		}
		return Graph;
	}

	static UPCGNode* FindPcgNode(UPCGGraph* Graph, const TSharedRef<FJsonObject>& Args, FString& Error)
	{
		FString Token;
		if (!Args->TryGetStringField(TEXT("node_name"), Token)) Args->TryGetStringField(TEXT("node"), Token);
		if (Token.IsEmpty()) Args->TryGetStringField(TEXT("node_title"), Token);
		int32 RequestedIndex = INDEX_NONE;
		Args->TryGetNumberField(TEXT("node_index"), RequestedIndex);
		for (int32 Index = 0; Graph && Index < Graph->GetNodes().Num(); ++Index)
		{
			UPCGNode* Node = Graph->GetNodes()[Index];
			if (!Node) continue;
			if (RequestedIndex == Index || (!Token.IsEmpty() &&
				(Node->GetName().Equals(Token, ESearchCase::IgnoreCase) ||
				 Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString().Equals(Token, ESearchCase::IgnoreCase))))
			{
				return Node;
			}
		}
		Error = FString::Printf(TEXT("PCG node was not found: %s (index=%d)."), *Token, RequestedIndex);
		return nullptr;
	}

	static TSharedRef<FJsonObject> PcgPinJson(const UPCGPin* Pin)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		if (!Pin) return Row;
		Row->SetStringField(TEXT("label"), Pin->Properties.Label.ToString());
		TArray<TSharedPtr<FJsonValue>> AllowedTypeIds;
		for (const FPCGDataTypeBaseId& TypeId : Pin->Properties.AllowedTypes.GetIds())
		{
			AllowedTypeIds.Add(MakeShared<FJsonValueString>(TypeId.ToString()));
		}
		Row->SetArrayField(TEXT("allowed_type_ids"), AllowedTypeIds);
		Row->SetNumberField(TEXT("edge_count"), Pin->Edges.Num());
		Row->SetBoolField(TEXT("connected"), !Pin->Edges.IsEmpty());
		return Row;
	}

	static UPCGPin* FindPin(UPCGNode* Node, const FString& Label, const bool bOutput)
	{
		if (!Node) return nullptr;
		const TArray<TObjectPtr<UPCGPin>>& Pins = bOutput ? Node->GetOutputPins() : Node->GetInputPins();
		for (UPCGPin* Pin : Pins)
		{
			if (Pin && Pin->Properties.Label.ToString().Equals(Label, ESearchCase::IgnoreCase)) return Pin;
		}
		return nullptr;
	}

	static bool SplitPcgPinPath(const FString& PinPath, FString& NodeToken, FString& PinLabel)
	{
		FString Value = PinPath;
		Value.TrimStartAndEndInline();
		bool bSplit = Value.Split(TEXT("::"), &NodeToken, &PinLabel, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (!bSplit) bSplit = Value.Split(TEXT("|"), &NodeToken, &PinLabel, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (!bSplit) bSplit = Value.Split(TEXT("."), &NodeToken, &PinLabel, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		NodeToken.TrimStartAndEndInline();
		PinLabel.TrimStartAndEndInline();
		return bSplit && !NodeToken.IsEmpty() && !PinLabel.IsEmpty();
	}

	static bool MergePinPathAlias(
		const FString& PinPath,
		FString& NodeToken,
		FString& PinLabel,
		const TCHAR* Field,
		FString& Error)
	{
		if (PinPath.IsEmpty()) return true;
		FString PathNode;
		FString PathPin;
		if (!SplitPcgPinPath(PinPath, PathNode, PathPin))
		{
			Error = FString::Printf(TEXT("%s must be formatted as Node::Pin (legacy '|' and '.' separators are also accepted)."), Field);
			return false;
		}
		if ((!NodeToken.IsEmpty() && !NodeToken.Equals(PathNode, ESearchCase::IgnoreCase)) ||
			(!PinLabel.IsEmpty() && !PinLabel.Equals(PathPin, ESearchCase::IgnoreCase)))
		{
			Error = FString::Printf(TEXT("%s conflicts with the explicit node/pin fields."), Field);
			return false;
		}
		NodeToken = PathNode;
		PinLabel = PathPin;
		return true;
	}

	static bool SaveReloadPcg(
		const FSololmcpToolExecutionContext& Context,
		UPCGGraph* Graph,
		const int32 ExpectedNodes,
		const int32 ExpectedEdges,
		const FString& PreSerializedFingerprint,
		const FString& PreSemanticFingerprint,
		TSharedRef<FJsonObject>& Receipt,
		FString& Error)
	{
		if (!Graph)
		{
			Error = TEXT("Cannot save a null PCG graph.");
			return false;
		}
		const FString ObjectPath = Graph->GetPathName();
		const FString SemanticFingerprint = PcgGraphContentFingerprint(Graph);
		if (!PreSemanticFingerprint.IsEmpty() && SemanticFingerprint == PreSemanticFingerprint)
		{
			Error = TEXT("PCG mutation produced no semantic content change.");
			return false;
		}
		Graph->MarkPackageDirty();
		if (!Context.Services.SaveAsset(ObjectPath, false, Error)) return false;
		FString PackageFilename;
		FString SerializedFingerprint;
		TArray<uint8> SerializedBytes;
		if (!ResolvePackageBytes(ObjectPath, PackageFilename, SerializedBytes, SerializedFingerprint, Error)) return false;
		const int32 Nodes = Graph->GetNodes().Num();
		const int32 Edges = Graph->GetAllEdges().Num();
		Receipt->SetBoolField(TEXT("saved"), true);
		Receipt->SetBoolField(TEXT("reloaded"), false);
		Receipt->SetStringField(TEXT("save_verification_mode"), TEXT("serialized_package_fingerprint"));
		Receipt->SetBoolField(TEXT("same_object_reload_accepted"), false);
		Receipt->SetBoolField(TEXT("serialization_fingerprint_verified"), true);
		Receipt->SetStringField(TEXT("serialized_package_file"), PackageFilename);
		Receipt->SetStringField(TEXT("serialized_package_sha1"), SerializedFingerprint);
		Receipt->SetNumberField(TEXT("serialized_package_bytes"), static_cast<double>(SerializedBytes.Num()));
		Receipt->SetStringField(TEXT("semantic_content_sha1"), SemanticFingerprint);
		Receipt->SetNumberField(TEXT("verified_node_count"), Nodes);
		Receipt->SetNumberField(TEXT("verified_edge_count"), Edges);
		if (!PreSerializedFingerprint.IsEmpty() && SerializedFingerprint == PreSerializedFingerprint)
		{
			Receipt->SetBoolField(TEXT("serialization_fingerprint_verified"), false);
			Error = TEXT("PCG save did not change the serialized package fingerprint.");
			return false;
		}
		if (Nodes != ExpectedNodes || Edges != ExpectedEdges)
		{
			Receipt->SetBoolField(TEXT("serialization_fingerprint_verified"), false);
			Error = FString::Printf(TEXT("PCG serialization mismatch: expected nodes=%d edges=%d, read nodes=%d edges=%d."), ExpectedNodes, ExpectedEdges, Nodes, Edges);
			return false;
		}
		return true;
	}

	static bool ValidatePcg(
		FSololmcpToolRegistry& Registry,
		const FString& AssetPath,
		TSharedRef<FJsonObject>& Receipt,
		FString& Error)
	{
		if (!Registry.HasRegisteredTool(TEXT("pcg_graph_validate")))
		{
			Error = TEXT("pcg_graph_validate is unavailable; mutation cannot be accepted without compile/structure validation.");
			return false;
		}
		TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		FString Summary;
		const bool bOk = Registry.ExecuteTool(TEXT("pcg_graph_validate"), Args, Result, Summary, Error);
		Receipt->SetStringField(TEXT("compile_validation_tool"), TEXT("pcg_graph_validate"));
		Receipt->SetBoolField(TEXT("compile_validation_passed"), bOk);
		Receipt->SetObjectField(TEXT("compile_validation"), Result);
		if (!Summary.IsEmpty()) Receipt->SetStringField(TEXT("compile_validation_summary"), Summary);
		return bOk;
	}

	static bool RollbackAndVerifyPcg(
		const FSololmcpToolExecutionContext& Context,
		UPCGGraph* Graph,
		const FString& AssetPath,
		const FString& PrePackageFilename,
		const TArray<uint8>& PrePackageBytes,
		const FString& PreFingerprint,
		FScopedTransaction& Transaction,
		TSharedRef<FJsonObject>& Receipt,
		FString& Error)
	{
		const FString OriginalError = Error;
		Transaction.Cancel();
		Receipt->SetBoolField(TEXT("rolled_back"), true);
		Receipt->SetStringField(TEXT("pre_snapshot_sha1"), PreFingerprint);

		FString VerifiedFingerprint = PcgGraphContentFingerprint(Graph);
		if (VerifiedFingerprint != PreFingerprint)
		{
			if (PrePackageFilename.IsEmpty() || PrePackageBytes.IsEmpty() ||
				!FFileHelper::SaveArrayToFile(PrePackageBytes, *PrePackageFilename))
			{
				if (Graph) Graph->MarkPackageDirty();
				Receipt->SetBoolField(TEXT("rollback_verified"), false);
				Error = OriginalError + TEXT(" Rollback failed: the pre-operation serialized package could not be restored.");
				return false;
			}

			UPackage* Package = Graph ? Graph->GetOutermost() : FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(AssetPath));
			if (Package)
			{
				Package->SetDirtyFlag(false);
				TArray<UPackage*> PackagesToUnload{Package};
				UPackageTools::FUnloadPackageParams UnloadParams(PackagesToUnload);
				UnloadParams.bUnloadDirtyPackages = true;
				Graph = nullptr;
				if (!UPackageTools::UnloadPackages(UnloadParams))
				{
					Package->SetDirtyFlag(true);
					Receipt->SetBoolField(TEXT("rollback_verified"), false);
					Error = OriginalError + FString::Printf(TEXT(" Rollback failed to unload the mutated package: %s"),
						*UnloadParams.OutErrorMessage.ToString());
					return false;
				}
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);

			FString ReloadError;
			UObject* ReloadedObject = Context.Services.LoadAsset(AssetPath, ReloadError);
			UPCGGraphInterface* ReloadedInterface = Cast<UPCGGraphInterface>(ReloadedObject);
			UPCGGraph* ReloadedGraph = ReloadedInterface ? ReloadedInterface->GetMutablePCGGraph() : Cast<UPCGGraph>(ReloadedObject);
			if (!ReloadedGraph)
			{
				Receipt->SetBoolField(TEXT("rollback_verified"), false);
				Error = OriginalError + TEXT(" Rollback package was restored but PCG readback failed: ") + ReloadError;
				return false;
			}
			VerifiedFingerprint = PcgGraphContentFingerprint(ReloadedGraph);
			Receipt->SetObjectField(TEXT("rollback_snapshot"), PcgGraphSnapshot(ReloadedGraph));
		}
		else
		{
			if (Graph && Graph->GetOutermost()) Graph->GetOutermost()->SetDirtyFlag(false);
			Receipt->SetObjectField(TEXT("rollback_snapshot"), PcgGraphSnapshot(Graph));
		}

		const bool bVerified = !PreFingerprint.IsEmpty() && VerifiedFingerprint == PreFingerprint;
		Receipt->SetStringField(TEXT("rollback_snapshot_sha1"), VerifiedFingerprint);
		Receipt->SetBoolField(TEXT("rollback_verified"), bVerified);
		if (!bVerified)
		{
			Error = OriginalError + TEXT(" Rollback readback does not match the pre-operation snapshot.");
			return false;
		}
		Error = OriginalError;
		return false;
	}

	static bool FinalizePcgMutation(
		FSololmcpToolRegistry& Registry,
		const FSololmcpToolExecutionContext& Context,
		UPCGGraph* Graph,
		const FString& AssetPath,
		const FString& PrePackageFilename,
		const TArray<uint8>& PrePackageBytes,
		const FString& PreFingerprint,
		FScopedTransaction& Transaction,
		TSharedRef<FJsonObject>& Receipt,
		FString& Error)
	{
		const int32 ExpectedNodes = Graph->GetNodes().Num();
		const int32 ExpectedEdges = Graph->GetAllEdges().Num();
		Receipt->SetObjectField(TEXT("post_snapshot"), PcgGraphSnapshot(Graph));
		if (!ValidatePcg(Registry, AssetPath, Receipt, Error) ||
			!SaveReloadPcg(Context, Graph, ExpectedNodes, ExpectedEdges, Sha1Hex(PrePackageBytes),
				PreFingerprint, Receipt, Error))
		{
			return RollbackAndVerifyPcg(Context, Graph, AssetPath, PrePackageFilename, PrePackageBytes,
				PreFingerprint, Transaction, Receipt, Error);
		}
		Receipt->SetBoolField(TEXT("rolled_back"), false);
		Receipt->SetBoolField(TEXT("success"), true);
		Receipt->SetStringField(TEXT("status"), TEXT("completed"));
		Receipt->SetArrayField(TEXT("delivery_gates_remaining"), StringsJson({TEXT("authoring_visual_qa_capture"), TEXT("authoring_runtime_smoke_submit/get"), TEXT("authoring_delivery_gate")}));
		return true;
	}

	static bool ReceiptPassed(const TSharedPtr<FJsonObject>& Receipt, FString& Reason)
	{
		if (!Receipt.IsValid())
		{
			Reason = TEXT("missing_receipt");
			return false;
		}
		bool b = false;
		const bool bHasExplicitVerdict =
			Receipt->TryGetBoolField(TEXT("success"), b) ||
			Receipt->TryGetBoolField(TEXT("passed"), b) ||
			Receipt->TryGetBoolField(TEXT("valid"), b);
		if (!bHasExplicitVerdict)
		{
			Reason = TEXT("receipt_missing_explicit_verdict");
			return false;
		}
		if (!b)
		{
			Reason = TEXT("receipt_explicit_failure");
			return false;
		}
		FString Status;
		Receipt->TryGetStringField(TEXT("status"), Status);
		if (Status.Equals(TEXT("failed"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("blocked"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("rejected"), ESearchCase::IgnoreCase))
		{
			Reason = TEXT("receipt_status_not_acceptable");
			return false;
		}
		return true;
	}

	static bool RegisterSuccessfulReceipt(
		const FString& Tool,
		const TSharedRef<FJsonObject>& Args,
		const TSharedRef<FJsonObject>& Out,
		FString& Error)
	{
		const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr;
		if (!Out->TryGetObjectField(TEXT("receipt"), ReceiptPtr) || !ReceiptPtr || !ReceiptPtr->IsValid())
		{
			Error = TEXT("Successful native tool did not produce a receipt for registry insertion.");
			return false;
		}
		TSharedRef<FJsonObject> Receipt = ReceiptPtr->ToSharedRef();
		FString ReceiptId;
		if (!Receipt->TryGetStringField(TEXT("receipt_id"), ReceiptId) || ReceiptId.IsEmpty())
		{
			Error = TEXT("Successful native tool produced a receipt without receipt_id.");
			return false;
		}

		FString AssetPath;
		if (Tool.StartsWith(TEXT("pcg_graph_")))
		{
			FString PathError;
			if (!ResolvePcgAssetPath(Args, AssetPath, PathError))
			{
				Error = TEXT("Receipt registry could not bind the PCG target: ") + PathError;
				return false;
			}
		}
		else if (!Args->TryGetStringField(TEXT("target_asset"), AssetPath))
		{
			Args->TryGetStringField(TEXT("asset_path"), AssetPath);
		}
		AssetPath = CanonicalAssetPath(AssetPath);

		FString TransactionId;
		Args->TryGetStringField(TEXT("transaction_id"), TransactionId);
		if (TransactionId.IsEmpty()) Receipt->TryGetStringField(TEXT("transaction_id"), TransactionId);
		TransactionId.TrimStartAndEndInline();
		const FString ProjectName = FApp::GetProjectName();
		const FDateTime IssuedAt = FDateTime::UtcNow();
		Receipt->SetStringField(TEXT("registry_project_name"), ProjectName);
		Receipt->SetStringField(TEXT("registry_asset_path"), AssetPath);
		Receipt->SetStringField(TEXT("registry_transaction_id"), TransactionId);
		Receipt->SetStringField(TEXT("registry_issued_utc"), IssuedAt.ToIso8601());
		Receipt->SetBoolField(TEXT("registry_backed"), true);
		Receipt->SetStringField(TEXT("receipt_integrity_sha1"), Sha1Hex(JsonString(Receipt)));

		FRegisteredReceipt Record;
		Record.Receipt = MakeShared<FJsonObject>(*Receipt);
		Record.CanonicalJson = JsonString(Record.Receipt.ToSharedRef());
		Record.Tool = Tool;
		Record.ProjectName = ProjectName;
		Record.AssetPath = AssetPath;
		Record.TransactionId = TransactionId;
		Record.IssuedAt = IssuedAt;
		{
			FScopeLock Lock(&StateMutex);
			const FDateTime Cutoff = IssuedAt - FTimespan::FromSeconds(ReceiptLifetimeSeconds);
			for (auto It = ReceiptRegistry.CreateIterator(); It; ++It)
			{
				if (It.Value().IssuedAt < Cutoff) It.RemoveCurrent();
			}
			ReceiptRegistry.Add(ReceiptId, MoveTemp(Record));
		}
		return true;
	}

	static bool ToolMatchesDeliveryField(const FString& Field, const FString& Tool)
	{
		if (Field == TEXT("preflight_receipt")) return Tool == TEXT("authoring_preflight_gate");
		if (Field == TEXT("snapshot_receipt")) return Tool == TEXT("authoring_target_snapshot");
		if (Field == TEXT("save_reload_receipt")) return Tool == TEXT("authoring_asset_save_reload");
		if (Field == TEXT("compile_receipt")) return Tool == TEXT("authoring_compile_diagnostics");
		if (Field == TEXT("visual_receipt")) return Tool == TEXT("authoring_visual_qa_capture");
		if (Field == TEXT("runtime_receipt"))
		{
			return Tool == TEXT("authoring_runtime_smoke_submit");
		}
		return false;
	}

	static bool ResolveRegisteredDeliveryReceipt(
		const FString& Field,
		const TSharedPtr<FJsonObject>& Candidate,
		const FString& ProjectName,
		const FString& AssetPath,
		const FString& TransactionId,
		TSharedPtr<FJsonObject>& RegisteredReceipt,
		FString& Reason)
	{
		FString ReceiptId;
		if (!Candidate.IsValid() || !Candidate->TryGetStringField(TEXT("receipt_id"), ReceiptId) || ReceiptId.IsEmpty())
		{
			Reason = TEXT("missing_registry_receipt_id");
			return false;
		}

		FRegisteredReceipt Record;
		{
			FScopeLock Lock(&StateMutex);
			const FRegisteredReceipt* Found = ReceiptRegistry.Find(ReceiptId);
			if (!Found)
			{
				Reason = TEXT("receipt_not_generated_in_this_process");
				return false;
			}
			Record = *Found;
		}

		if (!Record.Receipt.IsValid() || JsonString(Record.Receipt.ToSharedRef()) != Record.CanonicalJson)
		{
			Reason = TEXT("registry_receipt_integrity_mismatch");
			return false;
		}
		if (!ToolMatchesDeliveryField(Field, Record.Tool))
		{
			Reason = TEXT("registry_receipt_wrong_tool");
			return false;
		}
		if (!Record.ProjectName.Equals(ProjectName, ESearchCase::IgnoreCase))
		{
			Reason = TEXT("registry_receipt_project_mismatch");
			return false;
		}
		if (CanonicalAssetPath(Record.AssetPath) != CanonicalAssetPath(AssetPath))
		{
			Reason = TEXT("registry_receipt_asset_mismatch");
			return false;
		}
		if (Record.TransactionId != TransactionId || TransactionId.IsEmpty())
		{
			Reason = TEXT("registry_receipt_transaction_mismatch");
			return false;
		}
		const double AgeSeconds = (FDateTime::UtcNow() - Record.IssuedAt).GetTotalSeconds();
		if (AgeSeconds < -60.0 || AgeSeconds > ReceiptLifetimeSeconds)
		{
			Reason = TEXT("registry_receipt_expired_or_future_dated");
			return false;
		}
		if (!ReceiptPassed(Record.Receipt, Reason)) return false;
		RegisteredReceipt = Record.Receipt;
		return true;
	}

	static bool RunAuthoringTool(
		const FString& Name,
		FSololmcpToolRegistry& Registry,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		TSharedRef<FJsonObject> Receipt = BaseReceipt(Name);
		Out->SetObjectField(TEXT("receipt"), Receipt);
		FString AssetPath;
		if (!Args->TryGetStringField(TEXT("target_asset"), AssetPath)) Args->TryGetStringField(TEXT("asset_path"), AssetPath);

		if (Name == TEXT("authoring_preflight_gate"))
		{
			if (!TargetGuard(Args, AssetPath, true, Receipt, Error)) return false;
			bool bRequireTargetExists = true;
			bool AliasValue = true;
			const bool bHasPrimary = Args->TryGetBoolField(TEXT("require_target_exists"), bRequireTargetExists);
			const bool bHasAlias = Args->TryGetBoolField(TEXT("target_must_exist"), AliasValue);
			if (bHasPrimary && bHasAlias && bRequireTargetExists != AliasValue)
			{
				Error = TEXT("require_target_exists and target_must_exist conflict.");
				return false;
			}
			if (!bHasPrimary && bHasAlias) bRequireTargetExists = AliasValue;
			TArray<FString> RequiredTools;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Args->TryGetArrayField(TEXT("required_tools"), Values) && Values)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values) if (Value.IsValid()) RequiredTools.Add(Value->AsString());
			}
			TArray<FString> Missing;
			for (const FString& Tool : RequiredTools) if (!Registry.HasRegisteredTool(Tool)) Missing.Add(Tool);
			Receipt->SetArrayField(TEXT("required_tools"), StringsJson(RequiredTools));
			Receipt->SetArrayField(TEXT("missing_tools"), StringsJson(Missing));
			const bool bTargetExists = Context.Services.AssetExists(AssetPath);
			Receipt->SetBoolField(TEXT("require_target_exists"), bRequireTargetExists);
			Receipt->SetBoolField(TEXT("target_exists"), bTargetExists);
			if (bRequireTargetExists && !bTargetExists)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked"));
				Receipt->SetBoolField(TEXT("passed"), false);
				Receipt->SetStringField(TEXT("reason_code"), TEXT("blocked_required_target_missing"));
				Error = FString::Printf(TEXT("Authoring preflight requires an existing target, but none exists at %s."), *AssetPath);
				return false;
			}
			if (!Missing.IsEmpty())
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked"));
				Receipt->SetBoolField(TEXT("passed"), false);
				Receipt->SetStringField(TEXT("reason_code"), TEXT("blocked_required_tool_missing"));
				Error = FString::Printf(TEXT("Authoring preflight is blocked; %d required native tool(s) are missing."), Missing.Num());
				return false;
			}
			Receipt->SetBoolField(TEXT("success"), true);
			Summary = TEXT("Authoring preflight target and native capability gates passed.");
			return true;
		}

		if (Name == TEXT("authoring_schema_validate"))
		{
			TSharedPtr<FJsonObject> Payload;
			if (!ReadJsonObjectArgument(Args, TEXT("payload"), TEXT("payload_json"), Payload, Error)) return false;
			if (!Payload.IsValid())
			{
				Error = TEXT("payload or payload_json is required.");
				return false;
			}
			TArray<FString> Missing;
			const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
			if (Args->TryGetArrayField(TEXT("required_fields"), Required) && Required)
			{
				for (const TSharedPtr<FJsonValue>& Field : *Required)
				{
					const FString Key = Field->AsString();
					if (!Payload->HasField(Key)) Missing.Add(Key);
				}
			}
			Receipt->SetArrayField(TEXT("missing_fields"), StringsJson(Missing));
			Receipt->SetBoolField(TEXT("valid"), Missing.IsEmpty());
			Receipt->SetBoolField(TEXT("success"), Missing.IsEmpty());
			if (!Missing.IsEmpty())
			{
				Error = FString::Printf(TEXT("Schema validation failed; %d required field(s) are missing."), Missing.Num());
				return false;
			}
			Summary = TEXT("Authoring payload schema validation passed.");
			return true;
		}

		if (Name == TEXT("authoring_target_snapshot"))
		{
			if (!TargetGuard(Args, AssetPath, true, Receipt, Error)) return false;
			UObject* Target = Context.Services.LoadAsset(AssetPath, Error);
			if (!Target) return false;
			TSharedRef<FJsonObject> Snapshot = Cast<UPCGGraphInterface>(Target)
				? PcgGraphSnapshot(CastChecked<UPCGGraphInterface>(Target)->GetMutablePCGGraph())
				: ObjectSnapshot(Target);
			const FString SnapshotId = NewId(TEXT("snapshot"));
			Snapshot->SetStringField(TEXT("snapshot_id"), SnapshotId);
			Snapshot->SetStringField(TEXT("captured_utc"), UtcNow());
			{
				FScopeLock Lock(&StateMutex);
				Snapshots.Add(SnapshotId, Snapshot);
			}
			Receipt->SetStringField(TEXT("snapshot_id"), SnapshotId);
			Receipt->SetObjectField(TEXT("snapshot"), Snapshot);
			Receipt->SetBoolField(TEXT("success"), true);
			Summary = FString::Printf(TEXT("Captured native authoring snapshot %s."), *SnapshotId);
			return true;
		}

		if (Name == TEXT("authoring_transaction_begin"))
		{
			if (!TargetGuard(Args, AssetPath, true, Receipt, Error)) return false;
			FString TxId;
			if (!Args->TryGetStringField(TEXT("transaction_id"), TxId) || TxId.IsEmpty()) TxId = NewId(TEXT("tx"));
			FString Description;
			Args->TryGetStringField(TEXT("description"), Description);
			if (Description.IsEmpty()) Description = FString::Printf(TEXT("SOMOLMCP authoring %s"), *AssetPath);
			if (!SololmcpTransaction::FRegistry::Get().Begin(TxId, Description) && !SololmcpTransaction::FRegistry::Get().IsOpen(TxId))
			{
				Error = TEXT("Failed to begin authoring transaction.");
				return false;
			}
			Receipt->SetStringField(TEXT("transaction_id"), TxId);
			Receipt->SetBoolField(TEXT("open"), true);
			Receipt->SetBoolField(TEXT("success"), true);
			Summary = FString::Printf(TEXT("Authoring transaction %s is open."), *TxId);
			return true;
		}

		if (Name == TEXT("authoring_transaction_commit") || Name == TEXT("authoring_transaction_rollback"))
		{
			if (!TargetGuard(Args, AssetPath, true, Receipt, Error)) return false;
			FString TxId;
			if (!Args->TryGetStringField(TEXT("transaction_id"), TxId) || TxId.IsEmpty())
			{
				Error = TEXT("transaction_id is required.");
				return false;
			}
			const int32 Ops = SololmcpTransaction::FRegistry::Get().GetOpCount(TxId);
			const bool bCommit = Name.EndsWith(TEXT("commit"));
			const bool bOk = bCommit ? SololmcpTransaction::FRegistry::Get().End(TxId) : SololmcpTransaction::FRegistry::Get().Abort(TxId);
			if (!bOk)
			{
				Error = FString::Printf(TEXT("No open authoring transaction: %s"), *TxId);
				return false;
			}
			Receipt->SetStringField(TEXT("transaction_id"), TxId);
			Receipt->SetNumberField(TEXT("operation_count"), Ops);
			Receipt->SetBoolField(bCommit ? TEXT("committed") : TEXT("rolled_back"), true);
			Receipt->SetBoolField(TEXT("success"), true);
			Summary = FString::Printf(TEXT("Authoring transaction %s %s."), *TxId, bCommit ? TEXT("committed") : TEXT("rolled back"));
			return true;
		}

		if (Name == TEXT("authoring_asset_save_reload"))
		{
			if (!TargetGuard(Args, AssetPath, true, Receipt, Error)) return false;
			UObject* Before = Context.Services.LoadAsset(AssetPath, Error);
			if (!Before) return false;
			const FString BeforeClass = Before->GetClass()->GetPathName();
			if (!Context.Services.SaveAsset(AssetPath, false, Error)) return false;
			FString PackageFilename;
			FString SerializedFingerprint;
			TArray<uint8> SerializedBytes;
			const bool bMatch = ResolvePackageBytes(AssetPath, PackageFilename, SerializedBytes, SerializedFingerprint, Error);
			Receipt->SetBoolField(TEXT("saved"), true);
			Receipt->SetBoolField(TEXT("reloaded"), false);
			Receipt->SetBoolField(TEXT("same_object_reload_accepted"), false);
			Receipt->SetStringField(TEXT("save_verification_mode"), TEXT("serialized_package_fingerprint"));
			Receipt->SetStringField(TEXT("saved_class_path"), BeforeClass);
			Receipt->SetStringField(TEXT("serialized_package_file"), PackageFilename);
			Receipt->SetStringField(TEXT("serialized_package_sha1"), SerializedFingerprint);
			Receipt->SetNumberField(TEXT("serialized_package_bytes"), static_cast<double>(SerializedBytes.Num()));
			Receipt->SetBoolField(TEXT("serialization_fingerprint_verified"), bMatch);
			Receipt->SetBoolField(TEXT("success"), bMatch);
			if (!bMatch)
			{
				if (Error.IsEmpty()) Error = TEXT("Save verification could not fingerprint the serialized target package.");
				return false;
			}
			Summary = TEXT("Asset save serialization fingerprint gate passed without accepting a same-object reload.");
			return true;
		}

		if (Name == TEXT("authoring_compile_diagnostics"))
		{
			if (!TargetGuard(Args, AssetPath, true, Receipt, Error)) return false;
			UObject* Target = Context.Services.LoadAsset(AssetPath, Error);
			if (!Target) return false;
			FString CompileTool;
			Args->TryGetStringField(TEXT("compile_tool"), CompileTool);
			if (CompileTool.IsEmpty())
			{
				const FString ClassName = Target->GetClass()->GetName();
				if (ClassName.Contains(TEXT("Blueprint"))) CompileTool = TEXT("blueprint_repair_compile_gate");
				else if (ClassName.Contains(TEXT("Material"))) CompileTool = TEXT("material_recompile");
				else if (ClassName.Contains(TEXT("Niagara"))) CompileTool = TEXT("niagara_compile_diagnostics");
				else if (Cast<UPCGGraphInterface>(Target)) CompileTool = TEXT("pcg_graph_validate");
			}
			if (CompileTool.IsEmpty() || !Registry.HasRegisteredTool(CompileTool))
			{
				Error = TEXT("No native compile/validation tool is available for the target type.");
				return false;
			}
			TSharedPtr<FJsonObject> SuppliedCompileArgs;
			if (!ReadJsonObjectArgument(Args, TEXT("compile_args"), TEXT("compile_args_json"), SuppliedCompileArgs, Error)) return false;
			TSharedRef<FJsonObject> CompileArgs = SuppliedCompileArgs.IsValid()
				? MakeShared<FJsonObject>(*SuppliedCompileArgs)
				: MakeShared<FJsonObject>();
			if (!CompileArgs->HasField(TEXT("asset_path"))) CompileArgs->SetStringField(TEXT("asset_path"), AssetPath);
			if (CompileTool == TEXT("niagara_compile_diagnostics") && !CompileArgs->HasField(TEXT("system_asset_path"))) CompileArgs->SetStringField(TEXT("system_asset_path"), AssetPath);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			FString ChildSummary;
			const bool bOk = Registry.ExecuteTool(CompileTool, CompileArgs, Result, ChildSummary, Error);
			Receipt->SetStringField(TEXT("compile_tool"), CompileTool);
			Receipt->SetObjectField(TEXT("diagnostics"), Result);
			Receipt->SetBoolField(TEXT("success"), bOk);
			Receipt->SetBoolField(TEXT("compile_clean"), bOk);
			Summary = ChildSummary;
			return bOk;
		}

		if (Name == TEXT("authoring_visual_qa_capture"))
		{
			if (!TargetGuard(Args, AssetPath, true, Receipt, Error)) return false;
			int32 MaxWidth = 1920, MaxHeight = 1080;
			Args->TryGetNumberField(TEXT("max_width"), MaxWidth);
			Args->TryGetNumberField(TEXT("max_height"), MaxHeight);
			TArray<uint8> Png;
			if (!Context.Services.CaptureViewportScreenshot(Png, MaxWidth, MaxHeight, Error) || Png.IsEmpty()) return false;
			const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("AuthoringQA"));
			IFileManager::Get().MakeDirectory(*Directory, true);
			const FString File = FPaths::Combine(Directory, NewId(TEXT("visual_qa")) + TEXT(".png"));
			if (!FFileHelper::SaveArrayToFile(Png, *File))
			{
				Error = TEXT("Viewport capture succeeded but PNG persistence failed.");
				return false;
			}
			Receipt->SetStringField(TEXT("png_path"), File);
			Receipt->SetStringField(TEXT("sha1"), Sha1Hex(Png));
			Receipt->SetNumberField(TEXT("byte_count"), Png.Num());
			Receipt->SetBoolField(TEXT("success"), true);
			Receipt->SetBoolField(TEXT("visual_evidence_present"), true);
			Summary = FString::Printf(TEXT("Captured authoring visual QA evidence: %s"), *File);
			return true;
		}

		if (Name == TEXT("authoring_runtime_smoke_submit"))
		{
			if (!TargetGuard(Args, AssetPath, true, Receipt, Error)) return false;
			FString SmokeTool;
			if (!Args->TryGetStringField(TEXT("smoke_tool"), SmokeTool) || SmokeTool.IsEmpty() || SmokeTool.StartsWith(TEXT("authoring_runtime_smoke_")))
			{
				Error = TEXT("A non-recursive native smoke_tool is required.");
				return false;
			}
			if (!Registry.HasRegisteredTool(SmokeTool))
			{
				Error = FString::Printf(TEXT("Runtime smoke tool is not registered as native C++: %s"), *SmokeTool);
				return false;
			}
			TSharedPtr<FJsonObject> SuppliedSmokeArgs;
			if (!ReadJsonObjectArgument(Args, TEXT("smoke_args"), TEXT("smoke_args_json"), SuppliedSmokeArgs, Error)) return false;
			TSharedRef<FJsonObject> SmokeArgs = SuppliedSmokeArgs.IsValid()
				? MakeShared<FJsonObject>(*SuppliedSmokeArgs)
				: MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			FString ChildSummary, ChildError;
			const bool bOk = Registry.ExecuteTool(SmokeTool, SmokeArgs, Result, ChildSummary, ChildError);
			const FString SmokeId = NewId(TEXT("runtime_smoke"));
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("smoke_id"), SmokeId);
			State->SetStringField(TEXT("tool"), SmokeTool);
			State->SetStringField(TEXT("status"), bOk ? TEXT("succeeded") : TEXT("failed"));
			State->SetBoolField(TEXT("success"), bOk);
			State->SetObjectField(TEXT("result"), Result);
			State->SetStringField(TEXT("summary"), ChildSummary);
			if (!ChildError.IsEmpty()) State->SetStringField(TEXT("error"), ChildError);
			{
				FScopeLock Lock(&StateMutex);
				RuntimeSmokes.Add(SmokeId, State);
			}
			Receipt->SetStringField(TEXT("smoke_id"), SmokeId);
			Receipt->SetObjectField(TEXT("runtime_smoke"), State);
			Receipt->SetBoolField(TEXT("success"), bOk);
			Summary = bOk ? TEXT("Runtime smoke completed and was recorded.") : TEXT("Runtime smoke failed and was recorded.");
			if (!bOk) Error = ChildError.IsEmpty() ? TEXT("Runtime smoke failed.") : ChildError;
			return bOk;
		}

		if (Name == TEXT("authoring_runtime_smoke_get"))
		{
			if (!TargetGuard(Args, AssetPath, true, Receipt, Error)) return false;
			FString SmokeId;
			if (!Args->TryGetStringField(TEXT("smoke_id"), SmokeId) || SmokeId.IsEmpty())
			{
				Error = TEXT("smoke_id is required.");
				return false;
			}
			TSharedPtr<FJsonObject> State;
			{
				FScopeLock Lock(&StateMutex);
				State = RuntimeSmokes.FindRef(SmokeId);
			}
			if (!State.IsValid())
			{
				Error = FString::Printf(TEXT("Runtime smoke was not found: %s"), *SmokeId);
				return false;
			}
			Out->SetObjectField(TEXT("runtime_smoke"), State.ToSharedRef());
			bool bSuccess = false;
			State->TryGetBoolField(TEXT("success"), bSuccess);
			Receipt->SetBoolField(TEXT("success"), bSuccess);
			Summary = FString::Printf(TEXT("Returned runtime smoke %s."), *SmokeId);
			return bSuccess;
		}

		if (Name == TEXT("authoring_diff_report"))
		{
			FString BeforeId, AfterId;
			if (!Args->TryGetStringField(TEXT("before_snapshot_id"), BeforeId) || !Args->TryGetStringField(TEXT("after_snapshot_id"), AfterId))
			{
				Error = TEXT("before_snapshot_id and after_snapshot_id are required.");
				return false;
			}
			TSharedPtr<FJsonObject> Before, After;
			{
				FScopeLock Lock(&StateMutex);
				Before = Snapshots.FindRef(BeforeId);
				After = Snapshots.FindRef(AfterId);
			}
			if (!Before.IsValid() || !After.IsValid())
			{
				Error = TEXT("One or both snapshot ids were not found.");
				return false;
			}
			const FString BeforeText = JsonString(Before.ToSharedRef());
			const FString AfterText = JsonString(After.ToSharedRef());
			const bool bChanged = BeforeText != AfterText;
			Receipt->SetStringField(TEXT("before_sha1"), Sha1Hex(BeforeText));
			Receipt->SetStringField(TEXT("after_sha1"), Sha1Hex(AfterText));
			Receipt->SetBoolField(TEXT("changed"), bChanged);
			Receipt->SetBoolField(TEXT("success"), true);
			Summary = bChanged ? TEXT("Authoring snapshot diff detected changes.") : TEXT("Authoring snapshots are identical.");
			return true;
		}

		if (Name == TEXT("authoring_receipt_envelope") || Name == TEXT("authoring_delivery_gate"))
		{
			if (!TargetGuard(Args, AssetPath, true, Receipt, Error)) return false;
			if (!Context.Services.AssetExists(AssetPath))
			{
				Error = FString::Printf(TEXT("Authoring delivery target does not exist: %s"), *AssetPath);
				return false;
			}
			FString TransactionId;
			if (!Args->TryGetStringField(TEXT("transaction_id"), TransactionId) || TransactionId.TrimStartAndEnd().IsEmpty())
			{
				Error = TEXT("transaction_id is required to bind delivery receipts.");
				return false;
			}
			TransactionId.TrimStartAndEndInline();
			static const TCHAR* DeliveryFields[] = {
				TEXT("preflight_receipt"), TEXT("snapshot_receipt"), TEXT("save_reload_receipt"),
				TEXT("compile_receipt"), TEXT("visual_receipt"), TEXT("runtime_receipt")
			};
			TArray<FString> Missing, Rejected;
			TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
			for (const TCHAR* Field : DeliveryFields)
			{
				const TSharedPtr<FJsonObject>* Candidate = nullptr;
				if (!Args->TryGetObjectField(Field, Candidate) || !Candidate || !Candidate->IsValid())
				{
					Missing.Add(Field);
					continue;
				}
				FString Reason;
				TSharedPtr<FJsonObject> Registered;
				if (!ResolveRegisteredDeliveryReceipt(Field, *Candidate, FApp::GetProjectName(), AssetPath,
					TransactionId, Registered, Reason))
				{
					Rejected.Add(FString(Field) + TEXT(":") + Reason);
					continue;
				}
				Envelope->SetObjectField(Field, Registered.ToSharedRef());
			}
			const bool bPass = Missing.IsEmpty() && Rejected.IsEmpty();
			Receipt->SetObjectField(TEXT("evidence"), Envelope);
			Receipt->SetArrayField(TEXT("missing_receipts"), StringsJson(Missing));
			Receipt->SetArrayField(TEXT("rejected_receipts"), StringsJson(Rejected));
			Receipt->SetBoolField(TEXT("success"), bPass);
			Receipt->SetBoolField(TEXT("delivery_accepted"), bPass);
			Receipt->SetStringField(TEXT("status"), bPass ? TEXT("accepted") : TEXT("rejected"));
			if (!bPass)
			{
				Error = FString::Printf(TEXT("Authoring delivery rejected: %d missing and %d failed receipt(s)."), Missing.Num(), Rejected.Num());
				return false;
			}
			Summary = Name.EndsWith(TEXT("delivery_gate")) ? TEXT("Authoring delivery gate accepted all evidence.") : TEXT("Authoring receipt envelope is complete.");
			return true;
		}

		Error = FString::Printf(TEXT("Unsupported authoring QA tool: %s"), *Name);
		return false;
	}

	static TSharedRef<FJsonObject> AuthoringSchema()
	{
		const TSharedRef<FJsonObject> ClosedDynamicObject = FSololmcpSchemaBuilder::Object(
			{}, {}, TEXT("Legacy direct-call object; MCP callers should use the adjacent *_json field."), false);
		const TSharedRef<FJsonObject> ReceiptReference = FSololmcpSchemaBuilder::Object({
			{TEXT("receipt_id"), FSololmcpSchemaBuilder::String(TEXT("Process-local receipt id returned by a successful native tool."))}
		}, {TEXT("receipt_id")}, TEXT("Reference to a process-generated receipt."), false);
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target_project_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("project_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("target_asset"), FSololmcpSchemaBuilder::String()},
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("transaction_id"), FSololmcpSchemaBuilder::String()},
			{TEXT("description"), FSololmcpSchemaBuilder::String()},
			{TEXT("require_target_exists"), FSololmcpSchemaBuilder::Boolean(TEXT("Defaults true for preflight."))},
			{TEXT("target_must_exist"), FSololmcpSchemaBuilder::Boolean(TEXT("Legacy alias for require_target_exists."))},
			{TEXT("required_tools"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
			{TEXT("payload"), ClosedDynamicObject},
			{TEXT("payload_json"), FSololmcpSchemaBuilder::String(TEXT("JSON object payload for a recursively closed MCP schema."))},
			{TEXT("required_fields"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
			{TEXT("compile_tool"), FSololmcpSchemaBuilder::String()},
			{TEXT("compile_args"), ClosedDynamicObject},
			{TEXT("compile_args_json"), FSololmcpSchemaBuilder::String(TEXT("JSON object passed to the selected compile tool."))},
			{TEXT("max_width"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("max_height"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("smoke_tool"), FSololmcpSchemaBuilder::String()},
			{TEXT("smoke_args"), ClosedDynamicObject},
			{TEXT("smoke_args_json"), FSololmcpSchemaBuilder::String(TEXT("JSON object passed to the selected smoke tool."))},
			{TEXT("smoke_id"), FSololmcpSchemaBuilder::String()},
			{TEXT("before_snapshot_id"), FSololmcpSchemaBuilder::String()},
			{TEXT("after_snapshot_id"), FSololmcpSchemaBuilder::String()},
			{TEXT("preflight_receipt"), ReceiptReference},
			{TEXT("snapshot_receipt"), ReceiptReference},
			{TEXT("save_reload_receipt"), ReceiptReference},
			{TEXT("compile_receipt"), ReceiptReference},
			{TEXT("visual_receipt"), ReceiptReference},
			{TEXT("runtime_receipt"), ReceiptReference}
		}, {}, FString(), false);
	}

	static bool RunPcgTool(
		const FString& Name,
		FSololmcpToolRegistry& Registry,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		TSharedRef<FJsonObject> Receipt = BaseReceipt(Name);
		Receipt->SetStringField(TEXT("schema"), TEXT("somolmcp.pcg_native_authoring_receipt.v1"));
		Out->SetObjectField(TEXT("receipt"), Receipt);
		FString AssetPath;
		if (!ResolvePcgAssetPath(Args, AssetPath, Error)) return false;
		if (!TargetGuard(Args, AssetPath, true, Receipt, Error)) return false;

		if (Name == TEXT("pcg_graph_create"))
		{
			if (Context.Services.AssetExists(AssetPath))
			{
				Error = FString::Printf(TEXT("PCG graph already exists; replacement is fail-closed: %s"), *AssetPath);
				return false;
			}
			FString PackagePath, AssetName;
			if (!AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || AssetName.IsEmpty())
			{
				Error = TEXT("asset_path must include an asset name.");
				return false;
			}
			UObject* Created = Context.Services.CreateAsset(PackagePath, AssetName, TEXT("/Script/PCG.PCGGraph"), TEXT("/Script/PCGEditor.PCGGraphFactory"), nullptr, Error, false);
			UPCGGraphInterface* Interface = Cast<UPCGGraphInterface>(Created);
			UPCGGraph* Graph = Interface ? Interface->GetMutablePCGGraph() : Cast<UPCGGraph>(Created);
			if (!Graph)
			{
				if (Created) Context.Services.DeleteAsset(AssetPath, Error);
				Error = TEXT("PCGGraphFactory did not create a mutable UPCGGraph.");
				return false;
			}
			Receipt->SetObjectField(TEXT("pre_snapshot"), ObjectSnapshot(nullptr));
			if (!ValidatePcg(Registry, AssetPath, Receipt, Error) || !SaveReloadPcg(Context, Graph,
				Graph->GetNodes().Num(), Graph->GetAllEdges().Num(), FString(), FString(), Receipt, Error))
			{
				FString DeleteError;
				const bool bDeleted = Context.Services.DeleteAsset(AssetPath, DeleteError);
				const FString PackageFilename = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
				const bool bAbsent = !Context.Services.AssetExists(AssetPath) && !IFileManager::Get().FileExists(*PackageFilename);
				Receipt->SetBoolField(TEXT("rolled_back"), true);
				Receipt->SetBoolField(TEXT("rollback_delete_succeeded"), bDeleted);
				Receipt->SetBoolField(TEXT("rollback_verified"), bAbsent);
				Receipt->SetBoolField(TEXT("rollback_target_absent"), bAbsent);
				if (!DeleteError.IsEmpty()) Receipt->SetStringField(TEXT("rollback_error"), DeleteError);
				if (!bAbsent)
				{
					Error += TEXT(" Create rollback could not verify the pre-snapshot (target absent).");
				}
				return false;
			}
			Receipt->SetObjectField(TEXT("post_snapshot"), PcgGraphSnapshot(Graph));
			Receipt->SetBoolField(TEXT("success"), true);
			Receipt->SetStringField(TEXT("status"), TEXT("completed"));
			Summary = FString::Printf(TEXT("Created, validated, saved, and fingerprint-verified native PCG graph %s."), *AssetPath);
			return true;
		}

		UPCGGraph* Graph = LoadPcgGraph(Context, AssetPath, Error);
		if (!Graph) return false;

		if (Name == TEXT("pcg_graph_list_nodes") || Name == TEXT("pcg_graph_list_pins") || Name == TEXT("pcg_graph_get_node_info"))
		{
			Receipt->SetObjectField(TEXT("snapshot"), PcgGraphSnapshot(Graph));
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (UPCGNode* Node : Graph->GetNodes())
			{
				if (!Node) continue;
				if (Name == TEXT("pcg_graph_get_node_info"))
				{
					FString FindError;
					UPCGNode* Requested = FindPcgNode(Graph, Args, FindError);
					if (!Requested) { Error = FindError; return false; }
					if (Requested != Node) continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Node->GetName());
				Row->SetStringField(TEXT("title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
				Row->SetStringField(TEXT("settings_class"), Node->GetSettings() ? Node->GetSettings()->GetClass()->GetPathName() : FString());
				TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
				for (const UPCGPin* Pin : Node->GetInputPins()) Inputs.Add(MakeShared<FJsonValueObject>(PcgPinJson(Pin)));
				for (const UPCGPin* Pin : Node->GetOutputPins()) Outputs.Add(MakeShared<FJsonValueObject>(PcgPinJson(Pin)));
				if (Name != TEXT("pcg_graph_list_nodes"))
				{
					Row->SetArrayField(TEXT("input_pins"), Inputs);
					Row->SetArrayField(TEXT("output_pins"), Outputs);
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
				if (Name == TEXT("pcg_graph_get_node_info")) break;
			}
			Out->SetArrayField(Name == TEXT("pcg_graph_get_node_info") ? TEXT("nodes") : (Name == TEXT("pcg_graph_list_pins") ? TEXT("node_pins") : TEXT("nodes")), Rows);
			Out->SetNumberField(TEXT("count"), Rows.Num());
			Receipt->SetBoolField(TEXT("success"), true);
			Summary = FString::Printf(TEXT("%s returned %d native PCG row(s)."), *Name, Rows.Num());
			return true;
		}

		if (Graph->GetOutermost()->IsDirty())
		{
			Error = TEXT("PCG mutation is fail-closed because the target package has unsaved pre-existing changes; save it before retrying.");
			return false;
		}
		FString PrePackageFilename;
		FString PrePackageFingerprint;
		TArray<uint8> PrePackageBytes;
		if (!ResolvePackageBytes(AssetPath, PrePackageFilename, PrePackageBytes, PrePackageFingerprint, Error)) return false;
		const TSharedRef<FJsonObject> PreSnapshot = PcgGraphSnapshot(Graph);
		const FString PreFingerprint = PcgGraphContentFingerprint(Graph);
		Receipt->SetStringField(TEXT("pre_snapshot_sha1"), PreFingerprint);
		Receipt->SetStringField(TEXT("pre_serialized_package_sha1"), PrePackageFingerprint);
		FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("SOMOLMCP %s"), *Name)));
		Graph->Modify();
		Receipt->SetObjectField(TEXT("pre_snapshot"), PreSnapshot);
		auto FailMutation = [&]()
		{
			return RollbackAndVerifyPcg(Context, Graph, AssetPath, PrePackageFilename, PrePackageBytes,
				PreFingerprint, Transaction, Receipt, Error);
		};

		if (Name == TEXT("pcg_graph_add_node"))
		{
			FString ClassPath;
			if (!Args->TryGetStringField(TEXT("node_class_path"), ClassPath)) Args->TryGetStringField(TEXT("settings_class"), ClassPath);
			if (ClassPath.IsEmpty()) Args->TryGetStringField(TEXT("node_type"), ClassPath);
			UClass* SettingsClass = ClassPath.IsEmpty() ? nullptr : LoadObject<UClass>(nullptr, *ClassPath);
			if (!SettingsClass) SettingsClass = ClassPath.IsEmpty() ? nullptr : FindObject<UClass>(nullptr, *ClassPath);
			if (!SettingsClass || !SettingsClass->IsChildOf(UPCGSettings::StaticClass()))
			{
				Error = FString::Printf(TEXT("node_class_path must resolve to UPCGSettings: %s"), *ClassPath);
				return FailMutation();
			}
			UPCGSettings* Settings = nullptr;
			UPCGNode* Node = Graph->AddNodeOfType(SettingsClass, Settings);
			if (!Node || !Settings)
			{
				Error = TEXT("UPCGGraph::AddNodeOfType failed.");
				return FailMutation();
			}
			TSharedPtr<FJsonObject> Properties;
			if (!ReadJsonObjectArgument(Args, TEXT("properties"), TEXT("properties_json"), Properties, Error))
			{
				return FailMutation();
			}
			if (Properties.IsValid() && !Context.Services.ApplyProperties(Settings, Properties, Error))
			{
				return FailMutation();
			}
			FString NodeLabel;
			if (Args->TryGetStringField(TEXT("node_label"), NodeLabel) && !NodeLabel.TrimStartAndEnd().IsEmpty())
			{
				NodeLabel.TrimStartAndEndInline();
				Node->NodeTitle = FName(*NodeLabel);
			}
			Receipt->SetStringField(TEXT("created_node_name"), Node->GetName());
			Receipt->SetStringField(TEXT("settings_class"), SettingsClass->GetPathName());
		}
		else if (Name == TEXT("pcg_graph_remove_node"))
		{
			UPCGNode* Node = FindPcgNode(Graph, Args, Error);
			if (!Node) return FailMutation();
			const FString Removed = Node->GetName();
			Graph->RemoveNode(Node);
			Receipt->SetStringField(TEXT("removed_node_name"), Removed);
		}
		else if (Name == TEXT("pcg_graph_connect") || Name == TEXT("pcg_graph_disconnect"))
		{
			FString FromToken, ToToken, FromPinLabel, ToPinLabel;
			Args->TryGetStringField(TEXT("from_node"), FromToken);
			if (FromToken.IsEmpty()) Args->TryGetStringField(TEXT("source_node"), FromToken);
			Args->TryGetStringField(TEXT("to_node"), ToToken);
			if (ToToken.IsEmpty()) Args->TryGetStringField(TEXT("target_node"), ToToken);
			Args->TryGetStringField(TEXT("from_pin"), FromPinLabel);
			if (FromPinLabel.IsEmpty()) Args->TryGetStringField(TEXT("source_pin"), FromPinLabel);
			Args->TryGetStringField(TEXT("to_pin"), ToPinLabel);
			if (ToPinLabel.IsEmpty()) Args->TryGetStringField(TEXT("target_pin"), ToPinLabel);
			FString SourcePinPath, TargetPinPath;
			Args->TryGetStringField(TEXT("source_pin_path"), SourcePinPath);
			Args->TryGetStringField(TEXT("target_pin_path"), TargetPinPath);
			if (!MergePinPathAlias(SourcePinPath, FromToken, FromPinLabel, TEXT("source_pin_path"), Error) ||
				!MergePinPathAlias(TargetPinPath, ToToken, ToPinLabel, TEXT("target_pin_path"), Error))
			{
				return FailMutation();
			}
			if (FromToken.IsEmpty() || ToToken.IsEmpty() || FromPinLabel.IsEmpty() || ToPinLabel.IsEmpty())
			{
				Error = TEXT("Provide source/from node and pin plus target/to node and pin, or source_pin_path and target_pin_path.");
				return FailMutation();
			}
			TSharedRef<FJsonObject> FromArgs = MakeShared<FJsonObject>(); FromArgs->SetStringField(TEXT("node_name"), FromToken);
			TSharedRef<FJsonObject> ToArgs = MakeShared<FJsonObject>(); ToArgs->SetStringField(TEXT("node_name"), ToToken);
			UPCGNode* From = FindPcgNode(Graph, FromArgs, Error);
			UPCGNode* To = From ? FindPcgNode(Graph, ToArgs, Error) : nullptr;
			UPCGPin* FromPin = From ? FindPin(From, FromPinLabel, true) : nullptr;
			UPCGPin* ToPin = To ? FindPin(To, ToPinLabel, false) : nullptr;
			if (!From || !To || !FromPin || !ToPin)
			{
				if (Error.IsEmpty()) Error = TEXT("Requested PCG nodes or pins were not found.");
				return FailMutation();
			}
			bool bChanged = false;
			if (Name == TEXT("pcg_graph_connect"))
			{
				if (!FromPin->CanConnect(ToPin))
				{
					Error = TEXT("PCG pin type/direction compatibility rejected the connection.");
					return FailMutation();
				}
				bChanged = Graph->AddEdge(From, FName(*FromPinLabel), To, FName(*ToPinLabel)) != nullptr;
			}
			else
			{
				bChanged = Graph->RemoveEdge(From, FName(*FromPinLabel), To, FName(*ToPinLabel));
			}
			if (!bChanged)
			{
				Error = Name.EndsWith(TEXT("connect")) ? TEXT("PCG edge creation failed.") : TEXT("Requested PCG edge did not exist.");
				return FailMutation();
			}
			Receipt->SetStringField(TEXT("edge"), FString::Printf(TEXT("%s.%s -> %s.%s"), *FromToken, *FromPinLabel, *ToToken, *ToPinLabel));
		}
		else
		{
			Error = FString::Printf(TEXT("Unsupported native PCG replacement: %s"), *Name);
			return FailMutation();
		}

		if (!FinalizePcgMutation(Registry, Context, Graph, AssetPath, PrePackageFilename, PrePackageBytes,
			PreFingerprint, Transaction, Receipt, Error)) return false;
		Summary = FString::Printf(TEXT("%s completed with native mutation, validation, serialized-package fingerprint verification, and rollback protection."), *Name);
		return true;
	}

	static TSharedRef<FJsonObject> PcgSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target_project_name"), FSololmcpSchemaBuilder::String(TEXT("Exact active UE project name."))},
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG graph /Game asset path."))},
			{TEXT("graph_path"), FSololmcpSchemaBuilder::String(TEXT("Legacy alias for asset_path."))},
			{TEXT("package_path"), FSololmcpSchemaBuilder::String(TEXT("Legacy /Game package directory; use with asset_name."))},
			{TEXT("asset_name"), FSololmcpSchemaBuilder::String(TEXT("Legacy unqualified asset name; use with package_path."))},
			{TEXT("node_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("node"), FSololmcpSchemaBuilder::String()},
			{TEXT("node_title"), FSololmcpSchemaBuilder::String()},
			{TEXT("node_index"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("node_class_path"), FSololmcpSchemaBuilder::String(TEXT("Native UPCGSettings class path."))},
			{TEXT("node_type"), FSololmcpSchemaBuilder::String(TEXT("Legacy alias for node_class_path."))},
			{TEXT("node_label"), FSololmcpSchemaBuilder::String(TEXT("Legacy caller label retained for compatibility."))},
			{TEXT("settings_class"), FSololmcpSchemaBuilder::String()},
			{TEXT("properties"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Legacy direct-call property bag; MCP callers should use properties_json."), false)},
			{TEXT("properties_json"), FSololmcpSchemaBuilder::String(TEXT("Reflected settings properties encoded as a JSON object."))},
			{TEXT("from_node"), FSololmcpSchemaBuilder::String()},
			{TEXT("from_pin"), FSololmcpSchemaBuilder::String()},
			{TEXT("to_node"), FSololmcpSchemaBuilder::String()},
			{TEXT("to_pin"), FSololmcpSchemaBuilder::String()},
			{TEXT("source_node"), FSololmcpSchemaBuilder::String(TEXT("Legacy alias for from_node."))},
			{TEXT("source_pin"), FSololmcpSchemaBuilder::String(TEXT("Legacy alias for from_pin."))},
			{TEXT("target_node"), FSololmcpSchemaBuilder::String(TEXT("Legacy alias for to_node."))},
			{TEXT("target_pin"), FSololmcpSchemaBuilder::String(TEXT("Legacy alias for to_pin."))},
			{TEXT("source_pin_path"), FSololmcpSchemaBuilder::String(TEXT("Legacy Node::Pin source path."))},
			{TEXT("target_pin_path"), FSololmcpSchemaBuilder::String(TEXT("Legacy Node::Pin target path."))}
		}, {TEXT("target_project_name")}, FString(), false);
	}

	static void CloseSchemaObjectsRecursively(const TSharedRef<FJsonObject>& Schema)
	{
		FString Type;
		if (Schema->TryGetStringField(TEXT("type"), Type) && Type == TEXT("object"))
		{
			Schema->SetBoolField(TEXT("additionalProperties"), false);
		}

		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (Schema->TryGetObjectField(TEXT("properties"), Properties) && Properties && Properties->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
			{
				if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> Child = Pair.Value->AsObject();
					if (Child.IsValid()) CloseSchemaObjectsRecursively(Child.ToSharedRef());
				}
			}
		}

		const TSharedPtr<FJsonObject>* Items = nullptr;
		if (Schema->TryGetObjectField(TEXT("items"), Items) && Items && Items->IsValid())
		{
			CloseSchemaObjectsRecursively(Items->ToSharedRef());
		}
		for (const TCHAR* Keyword : {TEXT("oneOf"), TEXT("anyOf"), TEXT("allOf")})
		{
			const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
			if (!Schema->TryGetArrayField(Keyword, Children) || !Children) continue;
			for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
			{
				if (ChildValue.IsValid() && ChildValue->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> Child = ChildValue->AsObject();
					if (Child.IsValid()) CloseSchemaObjectsRecursively(Child.ToSharedRef());
				}
			}
		}
	}
}

void RegisterAuthoringQaPcgCompletionTools(FSololmcpToolRegistry& Registry)
{
	static const TCHAR* AuthoringNames[] = {
		TEXT("authoring_preflight_gate"),
		TEXT("authoring_schema_validate"),
		TEXT("authoring_target_snapshot"),
		TEXT("authoring_transaction_begin"),
		TEXT("authoring_transaction_commit"),
		TEXT("authoring_transaction_rollback"),
		TEXT("authoring_asset_save_reload"),
		TEXT("authoring_compile_diagnostics"),
		TEXT("authoring_visual_qa_capture"),
		TEXT("authoring_runtime_smoke_submit"),
		TEXT("authoring_runtime_smoke_get"),
		TEXT("authoring_diff_report"),
		TEXT("authoring_receipt_envelope"),
		TEXT("authoring_delivery_gate")
	};
	for (const TCHAR* NamePtr : AuthoringNames)
	{
		const FString Name(NamePtr);
		FSololmcpToolDefinition Tool;
		Tool.Name = Name;
		Tool.Description = FString::Printf(TEXT("Native C++ cross-domain authoring QA/delivery gate: %s"), *Name);
		Tool.InputSchema = AuthoringQaPcgCompletion::AuthoringSchema();
		AuthoringQaPcgCompletion::CloseSchemaObjectsRecursively(Tool.InputSchema);
		Tool.bUsesExternalPython = false;
		Tool.Execute = [Name, &Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			if (!AuthoringQaPcgCompletion::RunAuthoringTool(Name, Registry, Context, Args, Out, Summary, Error)) return false;
			return AuthoringQaPcgCompletion::RegisterSuccessfulReceipt(Name, Args, Out, Error);
		};
		Registry.Register(Tool);
	}

	// Audited against SololmcpLegacyPythonBackendNames.inl. These eight names
	// remain high-value PCG graph operations and are replaced by real native
	// UPCGGraph/UPCGNode/UPCGPin implementations rather than compatibility aliases.
	static const TCHAR* PcgNames[] = {
		TEXT("pcg_graph_create"),
		TEXT("pcg_graph_add_node"),
		TEXT("pcg_graph_remove_node"),
		TEXT("pcg_graph_connect"),
		TEXT("pcg_graph_disconnect"),
		TEXT("pcg_graph_list_nodes"),
		TEXT("pcg_graph_list_pins"),
		TEXT("pcg_graph_get_node_info")
	};
	for (const TCHAR* NamePtr : PcgNames)
	{
		const FString Name(NamePtr);
		FSololmcpToolDefinition Tool;
		Tool.Name = Name;
		Tool.Description = FString::Printf(TEXT("Native C++ replacement for retired Python PCG operation: %s"), *Name);
		Tool.InputSchema = AuthoringQaPcgCompletion::PcgSchema();
		AuthoringQaPcgCompletion::CloseSchemaObjectsRecursively(Tool.InputSchema);
		Tool.CacheTtlSeconds = (Name.Contains(TEXT("list")) || Name.Contains(TEXT("get"))) ? 2 : 0;
		Tool.bUsesExternalPython = false;
		Tool.Execute = [Name, &Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			if (!AuthoringQaPcgCompletion::RunPcgTool(Name, Registry, Context, Args, Out, Summary, Error)) return false;
			return AuthoringQaPcgCompletion::RegisterSuccessfulReceipt(Name, Args, Out, Error);
		};
		Registry.Register(Tool);
	}
}
}
