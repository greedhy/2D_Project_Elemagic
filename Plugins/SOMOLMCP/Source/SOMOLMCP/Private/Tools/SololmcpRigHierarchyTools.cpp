// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// ControlRig coverage — rig hierarchy transforms and element reflection.
//
// URigHierarchy is the second-largest single uncovered cluster on UE 5.8: 148
// BlueprintCallable entry points against which SOMOLMCP previously had exactly one
// tool (control_rig_list_elements, which returns names and types only). Everything
// that actually poses or measures a rig — local/global transforms, initial versus
// current, control values — was unreachable.
//
// The API is uniformly keyed by FRigElementKey{Name, Type}, so the same generic
// get/set pair that worked for Interchange nodes reaches the whole surface here:
//   GetLocalTransform(FRigElementKey, bInitial) / SetLocalTransform(...)
//   GetGlobalTransform(FRigElementKey, bInitial) / SetGlobalTransform(...)
// A per-element-type wrapper would add names without adding reach.
//
// Queue-first (SOMOLMCP_COMPLETE_SOLUTION.md 6.4): the _batch variants exist
// because posing a rig means touching dozens of elements at once, and the editor
// is game-thread bound with a small concurrent job budget — N single writes cost N
// game-thread entries while one batch costs one.
//
// bAffectChildren is surfaced rather than hardcoded: setting a parent transform
// with children attached is a different operation from setting it in isolation,
// and a rig tool that silently picked one would corrupt poses in ways that only
// show up downstream.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "Services/SololmcpEditorServices.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "Runtime/Launch/Resources/Version.h"

// UE 5.7 renamed ControlRigBlueprint.h to ControlRigBlueprintLegacy.h, the same
// rename RigVMBlueprint.h went through. UControlRigBlueprint does arrive
// transitively via ControlRigBlueprintEditorLibrary.h, but relying on that is how
// this codebase previously ended up with an incomplete type on an older engine, so
// the definition is included explicitly on both sides of the rename.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif
#include "ControlRigBlueprintEditorLibrary.h"
#include "Rigs/RigHierarchy.h"

namespace UE::SOMOLMCP
{

// Engine floor, measured: 5.5 and above build clean, 5.4 and 5.3 do not. The rig
// hierarchy APIs these tools call arrived after 5.4, so the module being present
// on older engines proves nothing about the API matching.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)

namespace RigHierarchyToolsPrivate
{
	/** Element type names accepted on the wire, matching ERigElementType. */
	inline TArray<FString> ElementTypeEnumValues()
	{
		return {
			TEXT("bone"), TEXT("null"), TEXT("control"),
			TEXT("curve"), TEXT("physics"), TEXT("socket"), TEXT("connector")
		};
	}

	inline bool ParseElementType(const FString& Value, ERigElementType& OutType)
	{
		if (Value.Equals(TEXT("bone"), ESearchCase::IgnoreCase))      { OutType = ERigElementType::Bone;      return true; }
		if (Value.Equals(TEXT("null"), ESearchCase::IgnoreCase))      { OutType = ERigElementType::Null;      return true; }
		if (Value.Equals(TEXT("control"), ESearchCase::IgnoreCase))   { OutType = ERigElementType::Control;   return true; }
		if (Value.Equals(TEXT("curve"), ESearchCase::IgnoreCase))     { OutType = ERigElementType::Curve;     return true; }
		if (Value.Equals(TEXT("physics"), ESearchCase::IgnoreCase))   { OutType = ERigElementType::Physics;   return true; }
		if (Value.Equals(TEXT("socket"), ESearchCase::IgnoreCase))    { OutType = ERigElementType::Socket;    return true; }
		if (Value.Equals(TEXT("connector"), ESearchCase::IgnoreCase)) { OutType = ERigElementType::Connector; return true; }
		return false;
	}

	inline FString ElementTypeToString(const ERigElementType Type)
	{
		return StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Type));
	}

	/** Resolve the hierarchy of a Control Rig Blueprint, with typed failures. */
	inline URigHierarchy* ResolveHierarchy(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		const TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		FString AssetPath;
		if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
			OutError = TEXT("Missing asset_path.");
			return nullptr;
		}
		UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
		if (Rig == nullptr)
		{
			SololmcpError::InvalidPath(OutStructured, AssetPath);
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not a Control Rig Blueprint."), *AssetPath);
			}
			return nullptr;
		}
		URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(Rig);
		if (Hierarchy == nullptr)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("asset_path"),
				TEXT("The blueprint has no hierarchy; it may need a recompile."));
			OutError = TEXT("Hierarchy unavailable.");
		}
		return Hierarchy;
	}

	/** Read {name, type} into a key, verifying the element exists. */
	inline bool ResolveKey(
		const URigHierarchy* Hierarchy,
		const TSharedPtr<FJsonObject>& Source,
		FRigElementKey& OutKey,
		FString& OutFailure)
	{
		FString Name;
		FString TypeName;
		if (!Source->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			OutFailure = TEXT("missing_name");
			return false;
		}
		if (!Source->TryGetStringField(TEXT("type"), TypeName) || TypeName.IsEmpty())
		{
			OutFailure = TEXT("missing_type");
			return false;
		}
		ERigElementType Type = ERigElementType::None;
		if (!ParseElementType(TypeName, Type))
		{
			OutFailure = TEXT("unknown_type");
			return false;
		}
		OutKey = FRigElementKey(FName(*Name), Type);
		if (!Hierarchy->Contains(OutKey))
		{
			OutFailure = TEXT("element_not_found");
			return false;
		}
		return true;
	}

	inline TSharedRef<FJsonObject> TransformToJson(const FTransform& Transform)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		const FVector Location = Transform.GetLocation();
		const FRotator Rotation = Transform.Rotator();
		const FVector Scale = Transform.GetScale3D();

		TSharedRef<FJsonObject> LocationJson = MakeShared<FJsonObject>();
		LocationJson->SetNumberField(TEXT("x"), Location.X);
		LocationJson->SetNumberField(TEXT("y"), Location.Y);
		LocationJson->SetNumberField(TEXT("z"), Location.Z);
		Json->SetObjectField(TEXT("location"), LocationJson);

		TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>();
		RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
		RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
		RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
		Json->SetObjectField(TEXT("rotation"), RotationJson);

		TSharedRef<FJsonObject> ScaleJson = MakeShared<FJsonObject>();
		ScaleJson->SetNumberField(TEXT("x"), Scale.X);
		ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
		ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
		Json->SetObjectField(TEXT("scale"), ScaleJson);
		return Json;
	}

	/** Parse a transform, leaving unspecified components at the current value. */
	inline bool JsonToTransform(
		const TSharedPtr<FJsonObject>& Source, const FTransform& Current, FTransform& OutTransform)
	{
		if (!Source.IsValid())
		{
			return false;
		}
		FVector Location = Current.GetLocation();
		FRotator Rotation = Current.Rotator();
		FVector Scale = Current.GetScale3D();

		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Source->TryGetObjectField(TEXT("location"), Object) && Object != nullptr)
		{
			(*Object)->TryGetNumberField(TEXT("x"), Location.X);
			(*Object)->TryGetNumberField(TEXT("y"), Location.Y);
			(*Object)->TryGetNumberField(TEXT("z"), Location.Z);
		}
		if (Source->TryGetObjectField(TEXT("rotation"), Object) && Object != nullptr)
		{
			(*Object)->TryGetNumberField(TEXT("pitch"), Rotation.Pitch);
			(*Object)->TryGetNumberField(TEXT("yaw"), Rotation.Yaw);
			(*Object)->TryGetNumberField(TEXT("roll"), Rotation.Roll);
		}
		if (Source->TryGetObjectField(TEXT("scale"), Object) && Object != nullptr)
		{
			(*Object)->TryGetNumberField(TEXT("x"), Scale.X);
			(*Object)->TryGetNumberField(TEXT("y"), Scale.Y);
			(*Object)->TryGetNumberField(TEXT("z"), Scale.Z);
		}
		OutTransform = FTransform(Rotation, Location, Scale);
		return true;
	}

	inline TSharedRef<FJsonObject> AssetArgSchema()
	{
		return FSololmcpSchemaBuilder::String(
			TEXT("Object path of the Control Rig Blueprint."));
	}

	inline TSharedRef<FJsonObject> SpaceArgSchema()
	{
		return FSololmcpSchemaBuilder::WithDefaultString(
			FSololmcpSchemaBuilder::String(
				TEXT("Transform space. 'global' is world-relative, 'local' is parent-relative."),
				{TEXT("global"), TEXT("local")}),
			TEXT("global"));
	}

	inline TSharedRef<FJsonObject> InitialArgSchema()
	{
		return FSololmcpSchemaBuilder::WithDefaultBoolean(
			FSololmcpSchemaBuilder::Boolean(
				TEXT("Address the initial (rest) transform instead of the current one.")),
			false);
	}
} // namespace RigHierarchyToolsPrivate

void RegisterRigHierarchyTools(FSololmcpToolRegistry& Registry)
{
	using namespace RigHierarchyToolsPrivate;

	// ── rig_hierarchy_element_list ─────────────────────────────────────────
	Registry.Register({
		TEXT("rig_hierarchy_element_list"),
		TEXT("List rig hierarchy elements with their parent and index, optionally filtered by type. "
			 "Unlike control_rig_list_elements this reports hierarchy structure, which is what a "
			 "caller needs before posing anything."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), AssetArgSchema()},
				{TEXT("element_type"), FSololmcpSchemaBuilder::String(
					TEXT("Restrict to one element type. Omit for all."), ElementTypeEnumValues())},
				{TEXT("limit"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(TEXT("Maximum elements to return.")), 500)}
			},
			{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			URigHierarchy* Hierarchy = ResolveHierarchy(Context, Args, OutStructured, OutError);
			if (Hierarchy == nullptr)
			{
				return false;
			}

			ERigElementType Filter = ERigElementType::All;
			FString TypeName;
			if (Args->TryGetStringField(TEXT("element_type"), TypeName) && !TypeName.IsEmpty())
			{
				if (!ParseElementType(TypeName, Filter))
				{
					SololmcpError::InvalidType(OutStructured, TEXT("element_type"),
						FString::Join(ElementTypeEnumValues(), TEXT("|")));
					OutError = FString::Printf(TEXT("Unknown element_type '%s'."), *TypeName);
					return false;
				}
			}

			int32 Limit = 500;
			Args->TryGetNumberField(TEXT("limit"), Limit);
			Limit = FMath::Clamp(Limit, 1, 20000);

			const TArray<FRigElementKey> Keys = Hierarchy->GetAllKeys(true, Filter);
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FRigElementKey& Key : Keys)
			{
				if (Rows.Num() >= Limit)
				{
					break;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Key.Name.ToString());
				Row->SetStringField(TEXT("type"), ElementTypeToString(Key.Type));
				Row->SetNumberField(TEXT("index"), Hierarchy->GetIndex(Key));

				const FRigElementKey ParentKey = Hierarchy->GetFirstParent(Key);
				if (ParentKey.IsValid())
				{
					Row->SetStringField(TEXT("parent_name"), ParentKey.Name.ToString());
					Row->SetStringField(TEXT("parent_type"), ElementTypeToString(ParentKey.Type));
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("elements"), Rows);
			OutStructured->SetNumberField(TEXT("returned"), Rows.Num());
			OutStructured->SetNumberField(TEXT("total"), Keys.Num());
			OutStructured->SetBoolField(TEXT("truncated"), Keys.Num() > Rows.Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d of %d hierarchy element(s)."), Rows.Num(), Keys.Num());
			return true;
		},
		nullptr,
		5
	});

	// ── rig_hierarchy_transform_get ────────────────────────────────────────
	Registry.Register({
		TEXT("rig_hierarchy_transform_get"),
		TEXT("Read one element's transform. Covers local and global space, current and initial "
			 "(rest) pose — the same four combinations the rig editor exposes."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), AssetArgSchema()},
				{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Element name."))},
				{TEXT("type"), FSololmcpSchemaBuilder::String(
					TEXT("Element type."), ElementTypeEnumValues())},
				{TEXT("space"), SpaceArgSchema()},
				{TEXT("initial"), InitialArgSchema()}
			},
			{TEXT("asset_path"), TEXT("name"), TEXT("type")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			URigHierarchy* Hierarchy = ResolveHierarchy(Context, Args, OutStructured, OutError);
			if (Hierarchy == nullptr)
			{
				return false;
			}

			FRigElementKey Key;
			FString Failure;
			if (!ResolveKey(Hierarchy, Args, Key, Failure))
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("name"),
					TEXT("Run rig_hierarchy_element_list to see the available elements."));
				OutStructured->SetStringField(TEXT("failure"), Failure);
				OutError = FString::Printf(TEXT("Element could not be resolved: %s."), *Failure);
				return false;
			}

			FString Space = TEXT("global");
			Args->TryGetStringField(TEXT("space"), Space);
			bool bInitial = false;
			Args->TryGetBoolField(TEXT("initial"), bInitial);
			const bool bGlobal = !Space.Equals(TEXT("local"), ESearchCase::IgnoreCase);

			const FTransform Transform = bGlobal
				? Hierarchy->GetGlobalTransform(Key, bInitial)
				: Hierarchy->GetLocalTransform(Key, bInitial);

			OutStructured->SetStringField(TEXT("name"), Key.Name.ToString());
			OutStructured->SetStringField(TEXT("type"), ElementTypeToString(Key.Type));
			OutStructured->SetStringField(TEXT("space"), bGlobal ? TEXT("global") : TEXT("local"));
			OutStructured->SetBoolField(TEXT("initial"), bInitial);
			OutStructured->SetObjectField(TEXT("transform"), TransformToJson(Transform));
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%s %s transform of %s."),
				bInitial ? TEXT("Initial") : TEXT("Current"),
				bGlobal ? TEXT("global") : TEXT("local"), *Key.Name.ToString());
			return true;
		},
		nullptr,
		0
	});

	// ── rig_hierarchy_transform_set ────────────────────────────────────────
	Registry.Register({
		TEXT("rig_hierarchy_transform_set"),
		TEXT("Set one element's transform. Components left out of the request keep their current "
			 "value, so a caller can move an element without restating its rotation and scale. "
			 "For posing several elements use rig_hierarchy_transform_set_batch."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), AssetArgSchema()},
				{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Element name."))},
				{TEXT("type"), FSololmcpSchemaBuilder::String(
					TEXT("Element type."), ElementTypeEnumValues())},
				{TEXT("space"), SpaceArgSchema()},
				{TEXT("initial"), InitialArgSchema()},
				{TEXT("affect_children"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Carry children along with the change. Defaults to true, matching the "
							 "rig editor; set false to move an element out from under its children.")),
					true)},
				{TEXT("transform"), FSololmcpSchemaBuilder::Object(
					{
						{TEXT("location"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("x, y, z"))},
						{TEXT("rotation"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("pitch, yaw, roll"))},
						{TEXT("scale"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("x, y, z"))}
					},
					{}, TEXT("Partial transform; omitted components are left unchanged."))}
			},
			{TEXT("asset_path"), TEXT("name"), TEXT("type"), TEXT("transform")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			URigHierarchy* Hierarchy = ResolveHierarchy(Context, Args, OutStructured, OutError);
			if (Hierarchy == nullptr)
			{
				return false;
			}

			FRigElementKey Key;
			FString Failure;
			if (!ResolveKey(Hierarchy, Args, Key, Failure))
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("name"),
					TEXT("Run rig_hierarchy_element_list to see the available elements."));
				OutStructured->SetStringField(TEXT("failure"), Failure);
				OutError = FString::Printf(TEXT("Element could not be resolved: %s."), *Failure);
				return false;
			}

			const TSharedPtr<FJsonObject>* TransformJson = nullptr;
			if (!Args->TryGetObjectField(TEXT("transform"), TransformJson) || TransformJson == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("transform"));
				OutError = TEXT("Missing transform.");
				return false;
			}

			FString Space = TEXT("global");
			Args->TryGetStringField(TEXT("space"), Space);
			bool bInitial = false;
			Args->TryGetBoolField(TEXT("initial"), bInitial);
			bool bAffectChildren = true;
			Args->TryGetBoolField(TEXT("affect_children"), bAffectChildren);
			const bool bGlobal = !Space.Equals(TEXT("local"), ESearchCase::IgnoreCase);

			const FTransform Current = bGlobal
				? Hierarchy->GetGlobalTransform(Key, bInitial)
				: Hierarchy->GetLocalTransform(Key, bInitial);
			FTransform Desired = Current;
			JsonToTransform(*TransformJson, Current, Desired);

			const FScopedTransaction Transaction(
				NSLOCTEXT("SOMOLMCP", "RigHierarchySetTransform", "SOMOLMCP Set Rig Element Transform"));
			if (bGlobal)
			{
				Hierarchy->SetGlobalTransform(Key, Desired, bInitial, bAffectChildren, /*bSetupUndo=*/true);
			}
			else
			{
				Hierarchy->SetLocalTransform(Key, Desired, bInitial, bAffectChildren, /*bSetupUndo=*/true);
			}

			// Read back rather than echoing the request: the hierarchy can clamp or
			// re-solve a value, and a caller that trusts the echo would not notice.
			const FTransform Applied = bGlobal
				? Hierarchy->GetGlobalTransform(Key, bInitial)
				: Hierarchy->GetLocalTransform(Key, bInitial);

			OutStructured->SetStringField(TEXT("name"), Key.Name.ToString());
			OutStructured->SetStringField(TEXT("type"), ElementTypeToString(Key.Type));
			OutStructured->SetStringField(TEXT("space"), bGlobal ? TEXT("global") : TEXT("local"));
			OutStructured->SetBoolField(TEXT("initial"), bInitial);
			OutStructured->SetBoolField(TEXT("affect_children"), bAffectChildren);
			OutStructured->SetObjectField(TEXT("applied_transform"), TransformToJson(Applied));
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Set %s %s transform of %s."),
				bInitial ? TEXT("initial") : TEXT("current"),
				bGlobal ? TEXT("global") : TEXT("local"), *Key.Name.ToString());
			return true;
		},
		nullptr,
		0
	});

	// ── rig_hierarchy_transform_get_batch ──────────────────────────────────
	Registry.Register({
		TEXT("rig_hierarchy_transform_get_batch"),
		TEXT("Read many element transforms in ONE game-thread entry. Use this to capture a pose "
			 "before a queued edit so it can be compared or restored afterwards."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), AssetArgSchema()},
				{TEXT("space"), SpaceArgSchema()},
				{TEXT("initial"), InitialArgSchema()},
				{TEXT("elements"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object(
						{
							{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Element name."))},
							{TEXT("type"), FSololmcpSchemaBuilder::String(
								TEXT("Element type."), ElementTypeEnumValues())}
						},
						{TEXT("name"), TEXT("type")}),
					TEXT("Elements to read. Omit to read every element."))}
			},
			{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			URigHierarchy* Hierarchy = ResolveHierarchy(Context, Args, OutStructured, OutError);
			if (Hierarchy == nullptr)
			{
				return false;
			}

			FString Space = TEXT("global");
			Args->TryGetStringField(TEXT("space"), Space);
			bool bInitial = false;
			Args->TryGetBoolField(TEXT("initial"), bInitial);
			const bool bGlobal = !Space.Equals(TEXT("local"), ESearchCase::IgnoreCase);

			TArray<FRigElementKey> Keys;
			const TArray<TSharedPtr<FJsonValue>>* Requested = nullptr;
			if (Args->TryGetArrayField(TEXT("elements"), Requested) && Requested != nullptr && Requested->Num() > 0)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Requested)
				{
					const TSharedPtr<FJsonObject>* Item = nullptr;
					if (!Value.IsValid() || !Value->TryGetObject(Item) || Item == nullptr)
					{
						continue;
					}
					FRigElementKey Key;
					FString Failure;
					if (ResolveKey(Hierarchy, *Item, Key, Failure))
					{
						Keys.Add(Key);
					}
				}
			}
			else
			{
				Keys = Hierarchy->GetAllKeys(true, ERigElementType::All);
			}

			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FRigElementKey& Key : Keys)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Key.Name.ToString());
				Row->SetStringField(TEXT("type"), ElementTypeToString(Key.Type));
				Row->SetObjectField(TEXT("transform"), TransformToJson(bGlobal
					? Hierarchy->GetGlobalTransform(Key, bInitial)
					: Hierarchy->GetLocalTransform(Key, bInitial)));
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("transforms"), Rows);
			OutStructured->SetNumberField(TEXT("count"), Rows.Num());
			OutStructured->SetStringField(TEXT("space"), bGlobal ? TEXT("global") : TEXT("local"));
			OutStructured->SetBoolField(TEXT("initial"), bInitial);
			OutStructured->SetNumberField(TEXT("game_thread_entries"), 1);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Read %d transform(s) in one game-thread entry."), Rows.Num());
			return true;
		},
		nullptr,
		0
	});

	// ── rig_hierarchy_transform_set_batch ──────────────────────────────────
	Registry.Register({
		TEXT("rig_hierarchy_transform_set_batch"),
		TEXT("Apply many element transforms in ONE game-thread entry, under a single undo step. "
			 "This is how a pose is set from a queued workload: posing a rig element by element "
			 "costs one game-thread entry each against a small concurrent job budget, and leaves "
			 "one undo entry per element instead of one per pose."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), AssetArgSchema()},
				{TEXT("space"), SpaceArgSchema()},
				{TEXT("initial"), InitialArgSchema()},
				{TEXT("affect_children"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Carry children along with each change.")), true)},
				{TEXT("stop_on_error"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Abort the remaining elements after the first failure. Defaults to false "
							 "so one bad element name does not discard a whole pose.")),
					false)},
				{TEXT("items"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object(
						{
							{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Element name."))},
							{TEXT("type"), FSololmcpSchemaBuilder::String(
								TEXT("Element type."), ElementTypeEnumValues())},
							{TEXT("transform"), FSololmcpSchemaBuilder::Object(
								{}, {}, TEXT("Partial transform; omitted components stay unchanged."))}
						},
						{TEXT("name"), TEXT("type"), TEXT("transform")}),
					TEXT("Element transforms to apply, in order."))}
			},
			{TEXT("asset_path"), TEXT("items")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			URigHierarchy* Hierarchy = ResolveHierarchy(Context, Args, OutStructured, OutError);
			if (Hierarchy == nullptr)
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

			FString Space = TEXT("global");
			Args->TryGetStringField(TEXT("space"), Space);
			bool bInitial = false;
			Args->TryGetBoolField(TEXT("initial"), bInitial);
			bool bAffectChildren = true;
			Args->TryGetBoolField(TEXT("affect_children"), bAffectChildren);
			bool bStopOnError = false;
			Args->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);
			const bool bGlobal = !Space.Equals(TEXT("local"), ESearchCase::IgnoreCase);

			// One transaction for the whole wave, so the pose undoes as a unit.
			const FScopedTransaction Transaction(
				NSLOCTEXT("SOMOLMCP", "RigHierarchySetTransformBatch", "SOMOLMCP Set Rig Pose"));

			TArray<TSharedPtr<FJsonValue>> Results;
			int32 Applied = 0;
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

				FRigElementKey Key;
				FString Failure;
				if (!ResolveKey(Hierarchy, *Item, Key, Failure))
				{
					FString Name;
					(*Item)->TryGetStringField(TEXT("name"), Name);
					Row->SetStringField(TEXT("name"), Name);
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), Failure);
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}
				Row->SetStringField(TEXT("name"), Key.Name.ToString());
				Row->SetStringField(TEXT("type"), ElementTypeToString(Key.Type));

				const TSharedPtr<FJsonObject>* TransformJson = nullptr;
				if (!(*Item)->TryGetObjectField(TEXT("transform"), TransformJson) || TransformJson == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("missing_transform"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				const FTransform Current = bGlobal
					? Hierarchy->GetGlobalTransform(Key, bInitial)
					: Hierarchy->GetLocalTransform(Key, bInitial);
				FTransform Desired = Current;
				JsonToTransform(*TransformJson, Current, Desired);

				if (bGlobal)
				{
					Hierarchy->SetGlobalTransform(Key, Desired, bInitial, bAffectChildren, /*bSetupUndo=*/true);
				}
				else
				{
					Hierarchy->SetLocalTransform(Key, Desired, bInitial, bAffectChildren, /*bSetupUndo=*/true);
				}

				Row->SetStringField(TEXT("status"), TEXT("ok"));
				Results.Add(MakeShared<FJsonValueObject>(Row));
				++Applied;
			}

			OutStructured->SetArrayField(TEXT("results"), Results);
			OutStructured->SetNumberField(TEXT("requested"), Items->Num());
			OutStructured->SetNumberField(TEXT("applied"), Applied);
			OutStructured->SetNumberField(TEXT("failed"), Failed);
			OutStructured->SetNumberField(TEXT("skipped"), Skipped);
			OutStructured->SetNumberField(TEXT("game_thread_entries"), 1);
			OutStructured->SetNumberField(TEXT("undo_entries"), 1);
			OutStructured->SetBoolField(TEXT("ok"), Failed == 0);
			OutSummary = FString::Printf(
				TEXT("Posed %d/%d element(s) in one game-thread entry and one undo step (%d failed, %d skipped)."),
				Applied, Items->Num(), Failed, Skipped);
			if (Failed > 0)
			{
				OutError = FString::Printf(TEXT("%d of %d element transforms failed."), Failed, Items->Num());
				return false;
			}
			return true;
		},
		nullptr,
		0
	});
}

#else

// Below the floor: register nothing, so tools/list reports absence rather than
// offering tools that cannot work here.
void RegisterRigHierarchyTools(FSololmcpToolRegistry&)
{
}

#endif

} // namespace UE::SOMOLMCP
