// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// Interchange coverage — Layer C (node-graph reflection), batch 2.
//
// Why reflection instead of one tool per accessor: of the ~1374 Interchange
// BlueprintCallable entry points on UE 5.8, roughly 877 are GetCustomXxx/
// SetCustomXxx pairs spread across the factory and translated node classes
// (UInterchangeMaterialFactoryNode alone has 95). All of them are built on the
// typed attribute storage that UInterchangeBaseNode exposes generically, so one
// get/set pair reaches every one of them. Verified present since UE 5.3.
//
// Queue-first design (see SOMOLMCP_COMPLETE_SOLUTION.md 6.1.1): the throughput
// ceiling for jobs/submit workloads is GameThread entries, not tool granularity —
// 300 named calls and 300 reflective calls cost the same 300 entries. So the
// batch variant is the point of this file, not a convenience: one entry for N
// mutations. interchange_node_attribute_catalog exists so a client can validate a
// whole queued wave before submitting it, instead of burning one of the four
// concurrent GameThread slots discovering a bad key at execution time.
//
// Mutation targets factory nodes on purpose. UInterchangeBaseNodeContainer::GetNode
// returns a const pointer while GetFactoryNode returns a mutable one, and factory
// nodes are what actually drive asset creation — so "read any node, write factory
// nodes" is both the compilable and the semantically correct split.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Runtime/Launch/Resources/Version.h"

#if defined(SOMOLMCP_HAS_INTERCHANGECORE) && SOMOLMCP_HAS_INTERCHANGECORE
#define SOMOLMCP_WITH_INTERCHANGE_NODES 1
#else
#define SOMOLMCP_WITH_INTERCHANGE_NODES 0
#endif

#if SOMOLMCP_WITH_INTERCHANGE_NODES
#include "Nodes/InterchangeBaseNode.h"
#include "Nodes/InterchangeBaseNodeContainer.h"
#include "Nodes/InterchangeFactoryBaseNode.h"
#endif

// UInterchangeBaseNodeContainer::RemoveNode / SetNamespace / GetIsAncestor
// arrived in 5.6; ReplaceFactoryNode / RemoveFactoryNode are 5.8-only.
#define SOMOLMCP_IX_CONTAINER_HAS_REMOVE_NODE \
	(ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6))

namespace UE::SOMOLMCP
{
namespace InterchangeNodeToolsPrivate
{
	inline FString EngineVersionString()
	{
		return FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	}

	inline bool RefuseNoModule(
		const TSharedRef<FJsonObject>& OutStructured, FString& OutError, const TCHAR* ToolName)
	{
		SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE_ON_ENGINE"), TEXT(""),
			TEXT("This build was configured without InterchangeCore."));
		OutStructured->SetStringField(TEXT("tool"), ToolName);
		OutStructured->SetStringField(TEXT("required_module"), TEXT("InterchangeCore"));
		OutStructured->SetStringField(TEXT("engine_version"), EngineVersionString());
		OutStructured->SetBoolField(TEXT("ok"), false);
		OutError = TEXT("InterchangeCore is not available in this build.");
		return false;
	}

	inline void AddStringArray(
		const TSharedRef<FJsonObject>& Object, const TCHAR* Field, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Json;
		Json.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Json.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(Field, Json);
	}

	/** Attribute value types the generic accessors cover. */
	enum class EAttrType : uint8
	{
		Unknown, Bool, Int32, Float, Double, String, Guid, LinearColor, Vector2
	};

	inline EAttrType ParseAttrType(const FString& Value)
	{
		if (Value.Equals(TEXT("bool"), ESearchCase::IgnoreCase))         { return EAttrType::Bool; }
		if (Value.Equals(TEXT("int32"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("int"), ESearchCase::IgnoreCase))       { return EAttrType::Int32; }
		if (Value.Equals(TEXT("float"), ESearchCase::IgnoreCase))        { return EAttrType::Float; }
		if (Value.Equals(TEXT("double"), ESearchCase::IgnoreCase))       { return EAttrType::Double; }
		if (Value.Equals(TEXT("string"), ESearchCase::IgnoreCase))       { return EAttrType::String; }
		if (Value.Equals(TEXT("guid"), ESearchCase::IgnoreCase))         { return EAttrType::Guid; }
		if (Value.Equals(TEXT("linearcolor"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("color"), ESearchCase::IgnoreCase))     { return EAttrType::LinearColor; }
		if (Value.Equals(TEXT("vector2"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("vector2f"), ESearchCase::IgnoreCase))  { return EAttrType::Vector2; }
		return EAttrType::Unknown;
	}

	inline const TCHAR* AttrTypeName(const EAttrType Type)
	{
		switch (Type)
		{
		case EAttrType::Bool:        return TEXT("bool");
		case EAttrType::Int32:       return TEXT("int32");
		case EAttrType::Float:       return TEXT("float");
		case EAttrType::Double:      return TEXT("double");
		case EAttrType::String:      return TEXT("string");
		case EAttrType::Guid:        return TEXT("guid");
		case EAttrType::LinearColor: return TEXT("linearcolor");
		case EAttrType::Vector2:     return TEXT("vector2");
		default:                     return TEXT("unknown");
		}
	}

	inline TArray<FString> AttrTypeEnumValues()
	{
		return {
			TEXT("bool"), TEXT("int32"), TEXT("float"), TEXT("double"),
			TEXT("string"), TEXT("guid"), TEXT("linearcolor"), TEXT("vector2")
		};
	}

#if SOMOLMCP_WITH_INTERCHANGE_NODES

	/**
	 * Server-side container handles. Interchange node containers are transient
	 * objects with no asset path, so a queued workload needs a stable handle to
	 * address the same graph across many jobs/submit calls. TStrongObjectPtr keeps
	 * them off the GC's reclaim list for as long as the client holds the handle.
	 */
	struct FContainerRegistry
	{
		TMap<FString, TStrongObjectPtr<UInterchangeBaseNodeContainer>> Containers;
		TMap<FString, FString> Origins;

		static FContainerRegistry& Get()
		{
			static FContainerRegistry Instance;
			return Instance;
		}

		FString Add(UInterchangeBaseNodeContainer* Container, const FString& Origin)
		{
			const FString Handle = FString::Printf(TEXT("ixc_%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower());
			Containers.Add(Handle, TStrongObjectPtr<UInterchangeBaseNodeContainer>(Container));
			Origins.Add(Handle, Origin);
			return Handle;
		}

		UInterchangeBaseNodeContainer* Find(const FString& Handle) const
		{
			const TStrongObjectPtr<UInterchangeBaseNodeContainer>* Found = Containers.Find(Handle);
			return Found != nullptr ? Found->Get() : nullptr;
		}

		bool Release(const FString& Handle)
		{
			Origins.Remove(Handle);
			return Containers.Remove(Handle) > 0;
		}
	};

	/** Resolve a container handle, filling a typed error when it is unknown. */
	inline UInterchangeBaseNodeContainer* ResolveContainer(
		const TSharedRef<FJsonObject>& Args,
		const TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		FString Handle;
		if (!Args->TryGetStringField(TEXT("container"), Handle) || Handle.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("container"));
			OutError = TEXT("Missing container handle.");
			return nullptr;
		}
		UInterchangeBaseNodeContainer* Container = FContainerRegistry::Get().Find(Handle);
		if (Container == nullptr)
		{
			SololmcpError::NotFound(OutStructured, Handle);
			OutStructured->SetStringField(TEXT("container"), Handle);
			OutStructured->SetStringField(TEXT("suggestion"),
				TEXT("Handles are per editor session. Call interchange_container_list to see live "
					 "handles, or interchange_container_create / _load to make one."));
			OutError = FString::Printf(TEXT("Unknown container handle '%s'."), *Handle);
			return nullptr;
		}
		return Container;
	}

	/** Read one attribute off a node into JSON. */
	inline bool ReadAttribute(
		const UInterchangeBaseNode* Node,
		const FString& Key,
		const EAttrType Type,
		const TSharedRef<FJsonObject>& Out,
		FString& OutFailure)
	{
		switch (Type)
		{
		case EAttrType::Bool:
		{
			bool Value = false;
			if (!Node->GetBooleanAttribute(Key, Value)) { OutFailure = TEXT("attribute_missing_or_type_mismatch"); return false; }
			Out->SetBoolField(TEXT("value"), Value);
			return true;
		}
		case EAttrType::Int32:
		{
			int32 Value = 0;
			if (!Node->GetInt32Attribute(Key, Value)) { OutFailure = TEXT("attribute_missing_or_type_mismatch"); return false; }
			Out->SetNumberField(TEXT("value"), Value);
			return true;
		}
		case EAttrType::Float:
		{
			float Value = 0.0f;
			if (!Node->GetFloatAttribute(Key, Value)) { OutFailure = TEXT("attribute_missing_or_type_mismatch"); return false; }
			Out->SetNumberField(TEXT("value"), Value);
			return true;
		}
		case EAttrType::Double:
		{
			double Value = 0.0;
			if (!Node->GetDoubleAttribute(Key, Value)) { OutFailure = TEXT("attribute_missing_or_type_mismatch"); return false; }
			Out->SetNumberField(TEXT("value"), Value);
			return true;
		}
		case EAttrType::String:
		{
			FString Value;
			if (!Node->GetStringAttribute(Key, Value)) { OutFailure = TEXT("attribute_missing_or_type_mismatch"); return false; }
			Out->SetStringField(TEXT("value"), Value);
			return true;
		}
		case EAttrType::Guid:
		{
			FGuid Value;
			if (!Node->GetGuidAttribute(Key, Value)) { OutFailure = TEXT("attribute_missing_or_type_mismatch"); return false; }
			Out->SetStringField(TEXT("value"), Value.ToString());
			return true;
		}
		case EAttrType::LinearColor:
		{
			FLinearColor Value;
			if (!Node->GetLinearColorAttribute(Key, Value)) { OutFailure = TEXT("attribute_missing_or_type_mismatch"); return false; }
			TSharedRef<FJsonObject> Color = MakeShared<FJsonObject>();
			Color->SetNumberField(TEXT("r"), Value.R);
			Color->SetNumberField(TEXT("g"), Value.G);
			Color->SetNumberField(TEXT("b"), Value.B);
			Color->SetNumberField(TEXT("a"), Value.A);
			Out->SetObjectField(TEXT("value"), Color);
			return true;
		}
		case EAttrType::Vector2:
		{
			FVector2f Value;
			if (!Node->GetVector2Attribute(Key, Value)) { OutFailure = TEXT("attribute_missing_or_type_mismatch"); return false; }
			TSharedRef<FJsonObject> Vec = MakeShared<FJsonObject>();
			Vec->SetNumberField(TEXT("x"), Value.X);
			Vec->SetNumberField(TEXT("y"), Value.Y);
			Out->SetObjectField(TEXT("value"), Vec);
			return true;
		}
		default:
			OutFailure = TEXT("unsupported_type");
			return false;
		}
	}

	/** Write one attribute onto a factory node from JSON. */
	inline bool WriteAttribute(
		UInterchangeFactoryBaseNode* Node,
		const FString& Key,
		const EAttrType Type,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutFailure)
	{
		if (!Value.IsValid())
		{
			OutFailure = TEXT("missing_value");
			return false;
		}
		switch (Type)
		{
		case EAttrType::Bool:
		{
			bool Typed = false;
			if (!Value->TryGetBool(Typed)) { OutFailure = TEXT("value_not_bool"); return false; }
			return Node->AddBooleanAttribute(Key, Typed) ? true : (OutFailure = TEXT("write_rejected"), false);
		}
		case EAttrType::Int32:
		{
			int32 Typed = 0;
			if (!Value->TryGetNumber(Typed)) { OutFailure = TEXT("value_not_int32"); return false; }
			return Node->AddInt32Attribute(Key, Typed) ? true : (OutFailure = TEXT("write_rejected"), false);
		}
		case EAttrType::Float:
		{
			double Typed = 0.0;
			if (!Value->TryGetNumber(Typed)) { OutFailure = TEXT("value_not_number"); return false; }
			return Node->AddFloatAttribute(Key, static_cast<float>(Typed)) ? true : (OutFailure = TEXT("write_rejected"), false);
		}
		case EAttrType::Double:
		{
			double Typed = 0.0;
			if (!Value->TryGetNumber(Typed)) { OutFailure = TEXT("value_not_number"); return false; }
			return Node->AddDoubleAttribute(Key, Typed) ? true : (OutFailure = TEXT("write_rejected"), false);
		}
		case EAttrType::String:
		{
			FString Typed;
			if (!Value->TryGetString(Typed)) { OutFailure = TEXT("value_not_string"); return false; }
			return Node->AddStringAttribute(Key, Typed) ? true : (OutFailure = TEXT("write_rejected"), false);
		}
		case EAttrType::Guid:
		{
			FString Typed;
			FGuid Parsed;
			if (!Value->TryGetString(Typed) || !FGuid::Parse(Typed, Parsed)) { OutFailure = TEXT("value_not_guid"); return false; }
			return Node->AddGuidAttribute(Key, Parsed) ? true : (OutFailure = TEXT("write_rejected"), false);
		}
		case EAttrType::LinearColor:
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value->TryGetObject(Object) || Object == nullptr) { OutFailure = TEXT("value_not_object"); return false; }
			FLinearColor Color(0.0f, 0.0f, 0.0f, 1.0f);
			(*Object)->TryGetNumberField(TEXT("r"), Color.R);
			(*Object)->TryGetNumberField(TEXT("g"), Color.G);
			(*Object)->TryGetNumberField(TEXT("b"), Color.B);
			(*Object)->TryGetNumberField(TEXT("a"), Color.A);
			return Node->AddLinearColorAttribute(Key, Color) ? true : (OutFailure = TEXT("write_rejected"), false);
		}
		case EAttrType::Vector2:
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value->TryGetObject(Object) || Object == nullptr) { OutFailure = TEXT("value_not_object"); return false; }
			double X = 0.0;
			double Y = 0.0;
			(*Object)->TryGetNumberField(TEXT("x"), X);
			(*Object)->TryGetNumberField(TEXT("y"), Y);
			return Node->AddVector2Attribute(Key, FVector2f(static_cast<float>(X), static_cast<float>(Y)))
				? true : (OutFailure = TEXT("write_rejected"), false);
		}
		default:
			OutFailure = TEXT("unsupported_type");
			return false;
		}
	}

	/** Summarize a node for list/inspect output. */
	inline TSharedRef<FJsonObject> DescribeNode(const UInterchangeBaseNode* Node)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("uid"), Node->GetUniqueID());
		Json->SetStringField(TEXT("display_label"), Node->GetDisplayLabel());
		Json->SetStringField(TEXT("type_name"), Node->GetTypeName());
		Json->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
		Json->SetStringField(TEXT("parent_uid"), Node->GetParentUid());
		return Json;
	}
#endif // SOMOLMCP_WITH_INTERCHANGE_NODES

	inline TSharedRef<FJsonObject> ContainerArgSchema()
	{
		return FSololmcpSchemaBuilder::String(
			TEXT("Container handle from interchange_container_create / _load."));
	}
} // namespace InterchangeNodeToolsPrivate

void RegisterInterchangeNodeTools(FSololmcpToolRegistry& Registry)
{
	using namespace InterchangeNodeToolsPrivate;

	// ── interchange_container_create ───────────────────────────────────────
	Registry.Register({
		TEXT("interchange_container_create"),
		TEXT("Create an empty Interchange node container and return a session handle. "
			 "Handles address the same graph across many jobs/submit calls."),
		FSololmcpSchemaBuilder::Object({}, {}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_container_create"));
#else
			UInterchangeBaseNodeContainer* Container =
				NewObject<UInterchangeBaseNodeContainer>(GetTransientPackage());
			if (Container == nullptr)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("NewObject<UInterchangeBaseNodeContainer> returned null."));
				OutError = TEXT("Could not create node container.");
				return false;
			}
			const FString Handle = FContainerRegistry::Get().Add(Container, TEXT("created"));
			OutStructured->SetStringField(TEXT("container"), Handle);
			OutStructured->SetStringField(TEXT("origin"), TEXT("created"));
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Created empty Interchange container %s."), *Handle);
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_container_load ─────────────────────────────────────────
	Registry.Register({
		TEXT("interchange_container_load"),
		TEXT("Load a previously saved Interchange node graph from disk and return a session handle."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("file_path"), FSololmcpSchemaBuilder::String(
					TEXT("Path to a file written by interchange_container_save."))}
			},
			{TEXT("file_path")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_container_load"));
#else
			FString FilePath;
			if (!Args->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("file_path"));
				OutError = TEXT("Missing file_path.");
				return false;
			}
			const FString AbsolutePath = FPaths::ConvertRelativePathToFull(FilePath);
			if (!IFileManager::Get().FileExists(*AbsolutePath))
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("file_path"),
					FString::Printf(TEXT("No such file: %s"), *AbsolutePath));
				OutError = FString::Printf(TEXT("File not found: %s"), *AbsolutePath);
				return false;
			}

			UInterchangeBaseNodeContainer* Container =
				NewObject<UInterchangeBaseNodeContainer>(GetTransientPackage());
			Container->LoadFromFile(AbsolutePath);

			TArray<FString> AllNodes;
			Container->GetNodes(UInterchangeBaseNode::StaticClass(), AllNodes);

			const FString Handle = FContainerRegistry::Get().Add(Container, AbsolutePath);
			OutStructured->SetStringField(TEXT("container"), Handle);
			OutStructured->SetStringField(TEXT("origin"), AbsolutePath);
			OutStructured->SetNumberField(TEXT("node_count"), AllNodes.Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Loaded %d node(s) into container %s."),
				AllNodes.Num(), *Handle);
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_container_save ─────────────────────────────────────────
	Registry.Register({
		TEXT("interchange_container_save"),
		TEXT("Write an Interchange node graph to disk so a later session can reload it."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("container"), ContainerArgSchema()},
				{TEXT("file_path"), FSololmcpSchemaBuilder::String(TEXT("Destination file path."))}
			},
			{TEXT("container"), TEXT("file_path")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_container_save"));
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}
			FString FilePath;
			if (!Args->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("file_path"));
				OutError = TEXT("Missing file_path.");
				return false;
			}
			const FString AbsolutePath = FPaths::ConvertRelativePathToFull(FilePath);
			Container->SaveToFile(AbsolutePath);

			const bool bWritten = IFileManager::Get().FileExists(*AbsolutePath);
			OutStructured->SetStringField(TEXT("file_path"), AbsolutePath);
			OutStructured->SetBoolField(TEXT("ok"), bWritten);
			if (!bWritten)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("file_path"),
					TEXT("SaveToFile produced no file; check the destination directory is writable."));
				OutError = FString::Printf(TEXT("Container save produced no file at %s."), *AbsolutePath);
				return false;
			}
			OutStructured->SetNumberField(TEXT("file_size_bytes"),
				static_cast<double>(IFileManager::Get().FileSize(*AbsolutePath)));
			OutSummary = FString::Printf(TEXT("Saved container to %s."), *AbsolutePath);
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_container_list ─────────────────────────────────────────
	Registry.Register({
		TEXT("interchange_container_list"),
		TEXT("List live Interchange container handles in this editor session."),
		FSololmcpSchemaBuilder::Object({}, {}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_container_list"));
#else
			FContainerRegistry& Registry2 = FContainerRegistry::Get();
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const TPair<FString, TStrongObjectPtr<UInterchangeBaseNodeContainer>>& Pair : Registry2.Containers)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("container"), Pair.Key);
				if (const FString* Origin = Registry2.Origins.Find(Pair.Key))
				{
					Row->SetStringField(TEXT("origin"), *Origin);
				}
				if (UInterchangeBaseNodeContainer* Container = Pair.Value.Get())
				{
					TArray<FString> Nodes;
					Container->GetNodes(UInterchangeBaseNode::StaticClass(), Nodes);
					Row->SetNumberField(TEXT("node_count"), Nodes.Num());
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			OutStructured->SetArrayField(TEXT("containers"), Rows);
			OutStructured->SetNumberField(TEXT("container_count"), Rows.Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d live Interchange container(s)."), Rows.Num());
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_container_release ──────────────────────────────────────
	Registry.Register({
		TEXT("interchange_container_release"),
		TEXT("Release a container handle so its graph can be garbage collected. "
			 "Call this at the end of a queued wave to avoid leaking graphs across a long session."),
		FSololmcpSchemaBuilder::Object(
			{{TEXT("container"), ContainerArgSchema()}}, {TEXT("container")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_container_release"));
#else
			FString Handle;
			if (!Args->TryGetStringField(TEXT("container"), Handle) || Handle.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("container"));
				OutError = TEXT("Missing container handle.");
				return false;
			}
			const bool bReleased = FContainerRegistry::Get().Release(Handle);
			OutStructured->SetStringField(TEXT("container"), Handle);
			OutStructured->SetBoolField(TEXT("released"), bReleased);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = bReleased
				? FString::Printf(TEXT("Released container %s."), *Handle)
				: FString::Printf(TEXT("Container %s was not live; nothing to release."), *Handle);
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_node_list ──────────────────────────────────────────────
	Registry.Register({
		TEXT("interchange_node_list"),
		TEXT("List nodes in a container, optionally filtered to a node class. "
			 "Use factory_only=true to get just the nodes that drive asset creation."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("container"), ContainerArgSchema()},
				{TEXT("node_class"), FSololmcpSchemaBuilder::String(
					TEXT("Class path to filter by, e.g. /Script/InterchangeFactoryNodes.InterchangeStaticMeshFactoryNode. "
						 "Omit for all nodes."))},
				{TEXT("factory_only"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Restrict to factory nodes.")), false)},
				{TEXT("limit"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(TEXT("Maximum rows to return.")), 200)}
			},
			{TEXT("container")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_list"));
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}

			bool bFactoryOnly = false;
			Args->TryGetBoolField(TEXT("factory_only"), bFactoryOnly);

			UClass* FilterClass = bFactoryOnly
				? UInterchangeFactoryBaseNode::StaticClass()
				: UInterchangeBaseNode::StaticClass();
			FString NodeClassPath;
			if (Args->TryGetStringField(TEXT("node_class"), NodeClassPath) && !NodeClassPath.IsEmpty())
			{
				UClass* Resolved = Cast<UClass>(FSoftObjectPath(NodeClassPath).TryLoad());
				if (Resolved == nullptr)
				{
					SololmcpError::NotFound(OutStructured, NodeClassPath);
					OutError = FString::Printf(TEXT("Could not resolve node_class '%s'."), *NodeClassPath);
					return false;
				}
				FilterClass = Resolved;
			}

			int32 Limit = 200;
			Args->TryGetNumberField(TEXT("limit"), Limit);
			Limit = FMath::Clamp(Limit, 1, 5000);

			TArray<FString> Uids;
			Container->GetNodes(FilterClass, Uids);

			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FString& Uid : Uids)
			{
				if (Rows.Num() >= Limit)
				{
					break;
				}
				if (const UInterchangeBaseNode* Node = Container->GetNode(Uid))
				{
					Rows.Add(MakeShared<FJsonValueObject>(DescribeNode(Node)));
				}
			}

			OutStructured->SetArrayField(TEXT("nodes"), Rows);
			OutStructured->SetNumberField(TEXT("returned"), Rows.Num());
			OutStructured->SetNumberField(TEXT("total_matching"), Uids.Num());
			OutStructured->SetBoolField(TEXT("truncated"), Uids.Num() > Rows.Num());
			OutStructured->SetStringField(TEXT("filter_class"), FilterClass->GetPathName());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d of %d node(s) matching %s."),
				Rows.Num(), Uids.Num(), *FilterClass->GetName());
			return true;
#endif
		},
		nullptr,
		5
	});

	// ── interchange_node_inspect ───────────────────────────────────────────
	Registry.Register({
		TEXT("interchange_node_inspect"),
		TEXT("Inspect one node: identity, class, parent, and children."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("container"), ContainerArgSchema()},
				{TEXT("node_uid"), FSololmcpSchemaBuilder::String(TEXT("Unique id of the node."))}
			},
			{TEXT("container"), TEXT("node_uid")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_inspect"));
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}
			FString NodeUid;
			if (!Args->TryGetStringField(TEXT("node_uid"), NodeUid) || NodeUid.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("node_uid"));
				OutError = TEXT("Missing node_uid.");
				return false;
			}
			const UInterchangeBaseNode* Node = Container->GetNode(NodeUid);
			if (Node == nullptr)
			{
				SololmcpError::NotFound(OutStructured, NodeUid);
				OutError = FString::Printf(TEXT("No node '%s' in this container."), *NodeUid);
				return false;
			}

			OutStructured = DescribeNode(Node);
			OutStructured->SetBoolField(TEXT("is_factory_node"),
				Container->GetFactoryNode(NodeUid) != nullptr);
			OutStructured->SetNumberField(TEXT("children_count"), Container->GetNodeChildrenCount(NodeUid));
			AddStringArray(OutStructured, TEXT("children_uids"), Container->GetNodeChildrenUids(NodeUid));
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Node '%s' (%s)."),
				*Node->GetDisplayLabel(), *Node->GetClass()->GetName());
			return true;
#endif
		},
		nullptr,
		5
	});

	// ── interchange_node_attribute_get ─────────────────────────────────────
	Registry.Register({
		TEXT("interchange_node_attribute_get"),
		TEXT("Read one typed attribute from any node. This reaches every GetCustomXxx accessor "
			 "across all Interchange node classes, because they are all backed by the same "
			 "typed attribute storage."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("container"), ContainerArgSchema()},
				{TEXT("node_uid"), FSololmcpSchemaBuilder::String(TEXT("Unique id of the node."))},
				{TEXT("key"), FSololmcpSchemaBuilder::String(
					TEXT("Attribute key, e.g. the XXX in GetCustomXXX. See interchange_node_attribute_catalog."))},
				{TEXT("type"), FSololmcpSchemaBuilder::String(
					TEXT("Value type to read as."), AttrTypeEnumValues())}
			},
			{TEXT("container"), TEXT("node_uid"), TEXT("key"), TEXT("type")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_attribute_get"));
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}
			FString NodeUid;
			FString Key;
			FString TypeName;
			Args->TryGetStringField(TEXT("node_uid"), NodeUid);
			Args->TryGetStringField(TEXT("key"), Key);
			Args->TryGetStringField(TEXT("type"), TypeName);
			if (NodeUid.IsEmpty() || Key.IsEmpty() || TypeName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured,
					NodeUid.IsEmpty() ? TEXT("node_uid") : (Key.IsEmpty() ? TEXT("key") : TEXT("type")));
				OutError = TEXT("node_uid, key and type are all required.");
				return false;
			}
			const EAttrType Type = ParseAttrType(TypeName);
			if (Type == EAttrType::Unknown)
			{
				SololmcpError::InvalidType(OutStructured, TEXT("type"),
					FString::Join(AttrTypeEnumValues(), TEXT("|")));
				OutError = FString::Printf(TEXT("Unknown attribute type '%s'."), *TypeName);
				return false;
			}
			const UInterchangeBaseNode* Node = Container->GetNode(NodeUid);
			if (Node == nullptr)
			{
				SololmcpError::NotFound(OutStructured, NodeUid);
				OutError = FString::Printf(TEXT("No node '%s' in this container."), *NodeUid);
				return false;
			}

			FString Failure;
			if (!ReadAttribute(Node, Key, Type, OutStructured, Failure))
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("key"),
					TEXT("The node has no attribute under that key with that type. "
						 "Run interchange_node_attribute_catalog for the node class."));
				OutStructured->SetStringField(TEXT("failure"), Failure);
				OutError = FString::Printf(TEXT("Attribute '%s' (%s) not readable on '%s'."),
					*Key, AttrTypeName(Type), *NodeUid);
				return false;
			}
			OutStructured->SetStringField(TEXT("node_uid"), NodeUid);
			OutStructured->SetStringField(TEXT("key"), Key);
			OutStructured->SetStringField(TEXT("type"), AttrTypeName(Type));
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Read %s.%s (%s)."), *NodeUid, *Key, AttrTypeName(Type));
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_node_attribute_set ─────────────────────────────────────
	Registry.Register({
		TEXT("interchange_node_attribute_set"),
		TEXT("Write one typed attribute onto a factory node. Factory nodes are what drive asset "
			 "creation, so this is how an import is retargeted before it runs. For more than a "
			 "couple of writes use interchange_node_attribute_set_batch instead."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("container"), ContainerArgSchema()},
				{TEXT("node_uid"), FSololmcpSchemaBuilder::String(TEXT("Unique id of a factory node."))},
				{TEXT("key"), FSololmcpSchemaBuilder::String(TEXT("Attribute key."))},
				{TEXT("type"), FSololmcpSchemaBuilder::String(
					TEXT("Value type."), AttrTypeEnumValues())},
				{TEXT("value"), FSololmcpSchemaBuilder::Empty()}
			},
			{TEXT("container"), TEXT("node_uid"), TEXT("key"), TEXT("type"), TEXT("value")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_attribute_set"));
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}
			FString NodeUid;
			FString Key;
			FString TypeName;
			Args->TryGetStringField(TEXT("node_uid"), NodeUid);
			Args->TryGetStringField(TEXT("key"), Key);
			Args->TryGetStringField(TEXT("type"), TypeName);
			const TSharedPtr<FJsonValue> Value = Args->TryGetField(TEXT("value"));
			if (NodeUid.IsEmpty() || Key.IsEmpty() || TypeName.IsEmpty() || !Value.IsValid())
			{
				SololmcpError::MissingParam(OutStructured,
					NodeUid.IsEmpty() ? TEXT("node_uid")
						: (Key.IsEmpty() ? TEXT("key")
						: (TypeName.IsEmpty() ? TEXT("type") : TEXT("value"))));
				OutError = TEXT("node_uid, key, type and value are all required.");
				return false;
			}
			const EAttrType Type = ParseAttrType(TypeName);
			if (Type == EAttrType::Unknown)
			{
				SololmcpError::InvalidType(OutStructured, TEXT("type"),
					FString::Join(AttrTypeEnumValues(), TEXT("|")));
				OutError = FString::Printf(TEXT("Unknown attribute type '%s'."), *TypeName);
				return false;
			}

			UInterchangeFactoryBaseNode* FactoryNode = Container->GetFactoryNode(NodeUid);
			if (FactoryNode == nullptr)
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("node_uid"),
					TEXT("Attribute writes target factory nodes. Use interchange_node_list with "
						 "factory_only=true to find writable nodes."));
				OutStructured->SetStringField(TEXT("node_uid"), NodeUid);
				OutError = FString::Printf(TEXT("'%s' is not a factory node in this container."), *NodeUid);
				return false;
			}

			FString Failure;
			if (!WriteAttribute(FactoryNode, Key, Type, Value, Failure))
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("value"), Failure);
				OutStructured->SetStringField(TEXT("failure"), Failure);
				OutError = FString::Printf(TEXT("Could not write '%s' (%s): %s."),
					*Key, AttrTypeName(Type), *Failure);
				return false;
			}
			OutStructured->SetStringField(TEXT("node_uid"), NodeUid);
			OutStructured->SetStringField(TEXT("key"), Key);
			OutStructured->SetStringField(TEXT("type"), AttrTypeName(Type));
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Set %s.%s (%s)."), *NodeUid, *Key, AttrTypeName(Type));
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_node_attribute_set_batch ───────────────────────────────
	Registry.Register({
		TEXT("interchange_node_attribute_set_batch"),
		TEXT("Write many attributes across many factory nodes in ONE game-thread entry. "
			 "This is the throughput tool for queued workloads: the editor is game-thread bound "
			 "with only a few concurrent job slots, so N single writes cost N entries while this "
			 "costs one. Per-item results are returned so a partial failure is diagnosable."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("container"), ContainerArgSchema()},
				{TEXT("items"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object(
						{
							{TEXT("node_uid"), FSololmcpSchemaBuilder::String(TEXT("Factory node id."))},
							{TEXT("key"), FSololmcpSchemaBuilder::String(TEXT("Attribute key."))},
							{TEXT("type"), FSololmcpSchemaBuilder::String(
								TEXT("Value type."), AttrTypeEnumValues())},
							{TEXT("value"), FSololmcpSchemaBuilder::Empty()}
						},
						{TEXT("node_uid"), TEXT("key"), TEXT("type"), TEXT("value")}),
					TEXT("Writes to apply, in order."))},
				{TEXT("stop_on_error"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Abort the remaining items on the first failure. Defaults to false so one "
							 "bad key does not discard an otherwise good wave.")),
					false)}
			},
			{TEXT("container"), TEXT("items")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_attribute_set_batch"));
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}
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

				FString NodeUid;
				FString Key;
				FString TypeName;
				(*Item)->TryGetStringField(TEXT("node_uid"), NodeUid);
				(*Item)->TryGetStringField(TEXT("key"), Key);
				(*Item)->TryGetStringField(TEXT("type"), TypeName);
				const TSharedPtr<FJsonValue> Value = (*Item)->TryGetField(TEXT("value"));

				Row->SetStringField(TEXT("node_uid"), NodeUid);
				Row->SetStringField(TEXT("key"), Key);

				const EAttrType Type = ParseAttrType(TypeName);
				if (NodeUid.IsEmpty() || Key.IsEmpty() || Type == EAttrType::Unknown || !Value.IsValid())
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("missing_or_invalid_fields"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				UInterchangeFactoryBaseNode* FactoryNode = Container->GetFactoryNode(NodeUid);
				if (FactoryNode == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("not_a_factory_node"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				FString Failure;
				if (WriteAttribute(FactoryNode, Key, Type, Value, Failure))
				{
					Row->SetStringField(TEXT("status"), TEXT("ok"));
					++Succeeded;
				}
				else
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), Failure);
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
				TEXT("Applied %d/%d attribute write(s) in one game-thread entry (%d failed, %d skipped)."),
				Succeeded, Items->Num(), Failed, Skipped);
			if (Failed > 0)
			{
				OutError = FString::Printf(TEXT("%d of %d attribute writes failed."), Failed, Items->Num());
				return false;
			}
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_node_attribute_catalog ─────────────────────────────────
	Registry.Register({
		TEXT("interchange_node_attribute_catalog"),
		TEXT("List the attribute keys a node class exposes, with their value types and whether they "
			 "are writable. Call this before queueing a wave of attribute writes so bad keys are "
			 "caught at planning time rather than after they have occupied a job slot."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("node_class"), FSololmcpSchemaBuilder::String(
					TEXT("Class path, e.g. /Script/InterchangeFactoryNodes.InterchangeStaticMeshFactoryNode."))},
				{TEXT("container"), FSololmcpSchemaBuilder::String(
					TEXT("Optional container handle; with node_uid, the class is taken from that node."))},
				{TEXT("node_uid"), FSololmcpSchemaBuilder::String(
					TEXT("Optional node id to derive the class from."))}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_attribute_catalog"));
#else
			UClass* NodeClass = nullptr;

			FString ContainerHandle;
			FString NodeUid;
			Args->TryGetStringField(TEXT("container"), ContainerHandle);
			Args->TryGetStringField(TEXT("node_uid"), NodeUid);
			if (!ContainerHandle.IsEmpty() && !NodeUid.IsEmpty())
			{
				if (UInterchangeBaseNodeContainer* Container = FContainerRegistry::Get().Find(ContainerHandle))
				{
					if (const UInterchangeBaseNode* Node = Container->GetNode(NodeUid))
					{
						NodeClass = Node->GetClass();
					}
				}
			}

			if (NodeClass == nullptr)
			{
				FString NodeClassPath;
				if (!Args->TryGetStringField(TEXT("node_class"), NodeClassPath) || NodeClassPath.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("node_class"));
					OutError = TEXT("Pass node_class, or container plus node_uid.");
					return false;
				}
				NodeClass = Cast<UClass>(FSoftObjectPath(NodeClassPath).TryLoad());
				if (NodeClass == nullptr)
				{
					SololmcpError::NotFound(OutStructured, NodeClassPath);
					OutError = FString::Printf(TEXT("Could not resolve node_class '%s'."), *NodeClassPath);
					return false;
				}
			}

			// The keys are derived from the class's own GetCustomXxx / SetCustomXxx
			// reflection rather than a hand-maintained table, so the catalog tracks
			// whatever the running engine actually ships.
			TMap<FString, TPair<FString, bool>> Keys; // key -> (type, writable)
			for (TFieldIterator<UFunction> It(NodeClass); It; ++It)
			{
				const UFunction* Function = *It;
				const FString Name = Function->GetName();
				const bool bGetter = Name.StartsWith(TEXT("GetCustom"));
				const bool bSetter = Name.StartsWith(TEXT("SetCustom"));
				if (!bGetter && !bSetter)
				{
					continue;
				}
				const FString Key = Name.RightChop(9); // strip "GetCustom" / "SetCustom"
				if (Key.IsEmpty())
				{
					continue;
				}

				FString TypeName = TEXT("unknown");
				for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
				{
					const FProperty* Param = *ParamIt;
					if (!Param->HasAnyPropertyFlags(CPF_Parm))
					{
						continue;
					}
					if (Param->IsA<FBoolProperty>())        { TypeName = TEXT("bool"); }
					else if (Param->IsA<FIntProperty>())    { TypeName = TEXT("int32"); }
					else if (Param->IsA<FFloatProperty>())  { TypeName = TEXT("float"); }
					else if (Param->IsA<FDoubleProperty>()) { TypeName = TEXT("double"); }
					else if (Param->IsA<FStrProperty>())    { TypeName = TEXT("string"); }
					else if (const FStructProperty* StructParam = CastField<FStructProperty>(Param))
					{
						const FString StructName = StructParam->Struct->GetName();
						if (StructName == TEXT("Guid"))              { TypeName = TEXT("guid"); }
						else if (StructName == TEXT("LinearColor"))  { TypeName = TEXT("linearcolor"); }
						else if (StructName.StartsWith(TEXT("Vector2"))) { TypeName = TEXT("vector2"); }
						else                                         { TypeName = StructName; }
					}
					break;
				}

				TPair<FString, bool>& Entry = Keys.FindOrAdd(Key, TPair<FString, bool>(TypeName, false));
				if (Entry.Key == TEXT("unknown") && TypeName != TEXT("unknown"))
				{
					Entry.Key = TypeName;
				}
				if (bSetter)
				{
					Entry.Value = true;
				}
			}

			TArray<FString> SortedKeys;
			Keys.GetKeys(SortedKeys);
			SortedKeys.Sort();

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 WritableCount = 0;
			for (const FString& Key : SortedKeys)
			{
				const TPair<FString, bool>& Entry = Keys[Key];
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("key"), Key);
				Row->SetStringField(TEXT("type"), Entry.Key);
				Row->SetBoolField(TEXT("writable"), Entry.Value);
				Row->SetBoolField(TEXT("generic_accessor_supported"),
					ParseAttrType(Entry.Key) != EAttrType::Unknown);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
				if (Entry.Value)
				{
					++WritableCount;
				}
			}

			OutStructured->SetStringField(TEXT("node_class"), NodeClass->GetPathName());
			OutStructured->SetArrayField(TEXT("attributes"), Rows);
			OutStructured->SetNumberField(TEXT("attribute_count"), Rows.Num());
			OutStructured->SetNumberField(TEXT("writable_count"), WritableCount);
			OutStructured->SetStringField(TEXT("engine_version"), EngineVersionString());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%s exposes %d attribute(s), %d writable."),
				*NodeClass->GetName(), Rows.Num(), WritableCount);
			return true;
#endif
		},
		nullptr,
		120
	});

	// ── interchange_node_attribute_get_batch ───────────────────────────────
	Registry.Register({
		TEXT("interchange_node_attribute_get_batch"),
		TEXT("Read many attributes across many nodes in ONE game-thread entry. Pair this with "
			 "_set_batch to diff a graph before and after a queued reconfiguration without paying "
			 "one job slot per read."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("container"), ContainerArgSchema()},
				{TEXT("items"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object(
						{
							{TEXT("node_uid"), FSololmcpSchemaBuilder::String(TEXT("Node id."))},
							{TEXT("key"), FSololmcpSchemaBuilder::String(TEXT("Attribute key."))},
							{TEXT("type"), FSololmcpSchemaBuilder::String(
								TEXT("Value type."), AttrTypeEnumValues())}
						},
						{TEXT("node_uid"), TEXT("key"), TEXT("type")}),
					TEXT("Reads to perform."))}
			},
			{TEXT("container"), TEXT("items")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_attribute_get_batch"));
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Args->TryGetArrayField(TEXT("items"), Items) || Items == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("items"));
				OutError = TEXT("Missing items array.");
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> Results;
			int32 Read = 0;
			int32 Failed = 0;

			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("index"), Index);

				const TSharedPtr<FJsonObject>* Item = nullptr;
				if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Item) || Item == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("item_not_object"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				FString NodeUid;
				FString Key;
				FString TypeName;
				(*Item)->TryGetStringField(TEXT("node_uid"), NodeUid);
				(*Item)->TryGetStringField(TEXT("key"), Key);
				(*Item)->TryGetStringField(TEXT("type"), TypeName);
				Row->SetStringField(TEXT("node_uid"), NodeUid);
				Row->SetStringField(TEXT("key"), Key);

				const EAttrType Type = ParseAttrType(TypeName);
				const UInterchangeBaseNode* Node = NodeUid.IsEmpty() ? nullptr : Container->GetNode(NodeUid);
				if (Node == nullptr || Key.IsEmpty() || Type == EAttrType::Unknown)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"),
						Node == nullptr ? TEXT("node_not_found") : TEXT("missing_or_invalid_fields"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				FString Failure;
				if (ReadAttribute(Node, Key, Type, Row, Failure))
				{
					Row->SetStringField(TEXT("status"), TEXT("ok"));
					Row->SetStringField(TEXT("type"), AttrTypeName(Type));
					++Read;
				}
				else
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), Failure);
					++Failed;
				}
				Results.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("results"), Results);
			OutStructured->SetNumberField(TEXT("requested"), Items->Num());
			OutStructured->SetNumberField(TEXT("read"), Read);
			OutStructured->SetNumberField(TEXT("failed"), Failed);
			OutStructured->SetNumberField(TEXT("game_thread_entries"), 1);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Read %d/%d attribute(s) in one game-thread entry."),
				Read, Items->Num());
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_node_attribute_remove ──────────────────────────────────
	Registry.Register({
		TEXT("interchange_node_attribute_remove"),
		TEXT("Remove an attribute from a factory node, reverting it to whatever the factory "
			 "defaults to for that key."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("container"), ContainerArgSchema()},
				{TEXT("node_uid"), FSololmcpSchemaBuilder::String(TEXT("Factory node id."))},
				{TEXT("key"), FSololmcpSchemaBuilder::String(TEXT("Attribute key to remove."))}
			},
			{TEXT("container"), TEXT("node_uid"), TEXT("key")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_attribute_remove"));
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}
			FString NodeUid;
			FString Key;
			Args->TryGetStringField(TEXT("node_uid"), NodeUid);
			Args->TryGetStringField(TEXT("key"), Key);
			if (NodeUid.IsEmpty() || Key.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, NodeUid.IsEmpty() ? TEXT("node_uid") : TEXT("key"));
				OutError = TEXT("node_uid and key are required.");
				return false;
			}
			UInterchangeFactoryBaseNode* FactoryNode = Container->GetFactoryNode(NodeUid);
			if (FactoryNode == nullptr)
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("node_uid"),
					TEXT("Attribute removal targets factory nodes."));
				OutError = FString::Printf(TEXT("'%s' is not a factory node."), *NodeUid);
				return false;
			}

			const bool bRemoved = FactoryNode->RemoveAttribute(Key);
			OutStructured->SetStringField(TEXT("node_uid"), NodeUid);
			OutStructured->SetStringField(TEXT("key"), Key);
			OutStructured->SetBoolField(TEXT("removed"), bRemoved);
			OutStructured->SetBoolField(TEXT("ok"), bRemoved);
			if (!bRemoved)
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("key"),
					TEXT("The node carries no attribute under that key."));
				OutError = FString::Printf(TEXT("No attribute '%s' on '%s'."), *Key, *NodeUid);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Removed attribute '%s' from %s."), *Key, *NodeUid);
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_node_roots_list ────────────────────────────────────────
	Registry.Register({
		TEXT("interchange_node_roots_list"),
		TEXT("List the root nodes of a container's hierarchy — the entry points for walking a "
			 "translated scene graph."),
		FSololmcpSchemaBuilder::Object(
			{{TEXT("container"), ContainerArgSchema()}}, {TEXT("container")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_roots_list"));
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}
			TArray<FString> Roots;
			Container->GetRoots(Roots);

			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FString& Uid : Roots)
			{
				if (const UInterchangeBaseNode* Node = Container->GetNode(Uid))
				{
					TSharedRef<FJsonObject> Row = DescribeNode(Node);
					Row->SetNumberField(TEXT("children_count"), Container->GetNodeChildrenCount(Uid));
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}
			}
			OutStructured->SetArrayField(TEXT("roots"), Rows);
			OutStructured->SetNumberField(TEXT("root_count"), Rows.Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d root node(s)."), Rows.Num());
			return true;
#endif
		},
		nullptr,
		5
	});

	// ── interchange_node_children_list ─────────────────────────────────────
	Registry.Register({
		TEXT("interchange_node_children_list"),
		TEXT("List the direct children of a node, for walking a translated scene hierarchy."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("container"), ContainerArgSchema()},
				{TEXT("node_uid"), FSololmcpSchemaBuilder::String(TEXT("Parent node id."))}
			},
			{TEXT("container"), TEXT("node_uid")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_children_list"));
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}
			FString NodeUid;
			if (!Args->TryGetStringField(TEXT("node_uid"), NodeUid) || NodeUid.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("node_uid"));
				OutError = TEXT("Missing node_uid.");
				return false;
			}
			if (!Container->IsNodeUidValid(NodeUid))
			{
				SololmcpError::NotFound(OutStructured, NodeUid);
				OutError = FString::Printf(TEXT("No node '%s' in this container."), *NodeUid);
				return false;
			}

			const TArray<FString> ChildUids = Container->GetNodeChildrenUids(NodeUid);
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FString& Uid : ChildUids)
			{
				if (const UInterchangeBaseNode* Child = Container->GetNode(Uid))
				{
					Rows.Add(MakeShared<FJsonValueObject>(DescribeNode(Child)));
				}
			}
			OutStructured->SetStringField(TEXT("node_uid"), NodeUid);
			OutStructured->SetArrayField(TEXT("children"), Rows);
			OutStructured->SetNumberField(TEXT("children_count"), Rows.Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d child node(s) under %s."), Rows.Num(), *NodeUid);
			return true;
#endif
		},
		nullptr,
		5
	});

	// ── interchange_node_parent_set ────────────────────────────────────────
	Registry.Register({
		TEXT("interchange_node_parent_set"),
		TEXT("Reparent a node within the container, or clear its parent by omitting parent_uid. "
			 "Use to restructure a translated hierarchy before the factories run."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("container"), ContainerArgSchema()},
				{TEXT("node_uid"), FSololmcpSchemaBuilder::String(TEXT("Node to reparent."))},
				{TEXT("parent_uid"), FSololmcpSchemaBuilder::String(
					TEXT("New parent node id. Omit or leave empty to clear the parent."))}
			},
			{TEXT("container"), TEXT("node_uid")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_parent_set"));
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}
			FString NodeUid;
			if (!Args->TryGetStringField(TEXT("node_uid"), NodeUid) || NodeUid.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("node_uid"));
				OutError = TEXT("Missing node_uid.");
				return false;
			}
			if (!Container->IsNodeUidValid(NodeUid))
			{
				SololmcpError::NotFound(OutStructured, NodeUid);
				OutError = FString::Printf(TEXT("No node '%s' in this container."), *NodeUid);
				return false;
			}

			FString ParentUid;
			Args->TryGetStringField(TEXT("parent_uid"), ParentUid);

			bool bApplied = false;
			if (ParentUid.IsEmpty())
			{
				// ClearNodeParentUid arrived with RemoveNode in 5.6. Older containers
				// expose no clear operation at all, so this reports rather than
				// silently leaving the node parented where the caller asked for a root.
#if SOMOLMCP_IX_CONTAINER_HAS_REMOVE_NODE
				bApplied = Container->ClearNodeParentUid(NodeUid);
				OutStructured->SetBoolField(TEXT("cleared"), true);
#else
				SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE_ON_ENGINE"), TEXT("parent_uid"),
					FString::Printf(
						TEXT("Clearing a parent needs UInterchangeBaseNodeContainer::ClearNodeParentUid ")
						TEXT("(UE 5.6+); this editor is UE %s. Pass an explicit parent_uid instead."),
						*EngineVersionString()));
				OutStructured->SetStringField(TEXT("required_api"),
					TEXT("UInterchangeBaseNodeContainer::ClearNodeParentUid"));
				OutStructured->SetStringField(TEXT("minimum_engine"), TEXT("5.6"));
				OutStructured->SetStringField(TEXT("engine_version"), EngineVersionString());
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutError = FString::Printf(
					TEXT("Clearing a node parent requires UE 5.6 (running %s)."), *EngineVersionString());
				return false;
#endif
			}
			else
			{
				if (!Container->IsNodeUidValid(ParentUid))
				{
					SololmcpError::NotFound(OutStructured, ParentUid);
					OutError = FString::Printf(TEXT("No parent node '%s' in this container."), *ParentUid);
					return false;
				}
				bApplied = Container->SetNodeParentUid(NodeUid, ParentUid);
				OutStructured->SetStringField(TEXT("parent_uid"), ParentUid);
			}

			OutStructured->SetStringField(TEXT("node_uid"), NodeUid);
			OutStructured->SetBoolField(TEXT("ok"), bApplied);
			if (!bApplied)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("parent_uid"),
					TEXT("The container refused the reparent; a cycle would be the usual cause."));
				OutError = FString::Printf(TEXT("Could not reparent '%s'."), *NodeUid);
				return false;
			}
			OutSummary = ParentUid.IsEmpty()
				? FString::Printf(TEXT("Cleared parent of %s."), *NodeUid)
				: FString::Printf(TEXT("Reparented %s under %s."), *NodeUid, *ParentUid);
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_node_remove (5.6+) ─────────────────────────────────────
	Registry.Register({
		TEXT("interchange_node_remove"),
		TEXT("Remove a node from the container so its factory never runs. Requires UE 5.6 or newer; "
			 "on older engines disable the node through its factory attributes instead."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("container"), ContainerArgSchema()},
				{TEXT("node_uid"), FSololmcpSchemaBuilder::String(TEXT("Node to remove."))}
			},
			{TEXT("container"), TEXT("node_uid")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_NODES
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_node_remove"));
#elif !SOMOLMCP_IX_CONTAINER_HAS_REMOVE_NODE
			SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE_ON_ENGINE"), TEXT(""),
				FString::Printf(
					TEXT("UInterchangeBaseNodeContainer::RemoveNode requires UE 5.6 or newer; this ")
					TEXT("editor is UE %s. Use interchange_node_attribute_set to disable the node ")
					TEXT("instead."),
					*EngineVersionString()));
			OutStructured->SetStringField(TEXT("tool"), TEXT("interchange_node_remove"));
			OutStructured->SetStringField(TEXT("required_api"),
				TEXT("UInterchangeBaseNodeContainer::RemoveNode"));
			OutStructured->SetStringField(TEXT("minimum_engine"), TEXT("5.6"));
			OutStructured->SetStringField(TEXT("engine_version"), EngineVersionString());
			OutStructured->SetStringField(TEXT("suggested_alternative"),
				TEXT("interchange_node_attribute_set"));
			OutStructured->SetBoolField(TEXT("ok"), false);
			OutError = FString::Printf(
				TEXT("interchange_node_remove requires UE 5.6 (running %s)."), *EngineVersionString());
			return false;
#else
			UInterchangeBaseNodeContainer* Container = ResolveContainer(Args, OutStructured, OutError);
			if (Container == nullptr)
			{
				return false;
			}
			FString NodeUid;
			if (!Args->TryGetStringField(TEXT("node_uid"), NodeUid) || NodeUid.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("node_uid"));
				OutError = TEXT("Missing node_uid.");
				return false;
			}
			if (!Container->IsNodeUidValid(NodeUid))
			{
				SololmcpError::NotFound(OutStructured, NodeUid);
				OutError = FString::Printf(TEXT("No node '%s' in this container."), *NodeUid);
				return false;
			}

			Container->RemoveNode(NodeUid);
			const bool bGone = !Container->IsNodeUidValid(NodeUid);
			OutStructured->SetStringField(TEXT("node_uid"), NodeUid);
			OutStructured->SetBoolField(TEXT("removed"), bGone);
			OutStructured->SetBoolField(TEXT("ok"), bGone);
			if (!bGone)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("node_uid"),
					TEXT("The container still reports the node as valid after RemoveNode."));
				OutError = FString::Printf(TEXT("Could not remove '%s'."), *NodeUid);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Removed node %s from the container."), *NodeUid);
			return true;
#endif
		},
		nullptr,
		0
	});
}

} // namespace UE::SOMOLMCP
