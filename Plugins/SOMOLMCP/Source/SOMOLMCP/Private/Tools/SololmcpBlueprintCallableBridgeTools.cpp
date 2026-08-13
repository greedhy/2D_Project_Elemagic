// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpBlueprintCallableBridgeTools.cpp
// ----------------------------------------------------------------------------
// UE 5.7-safe BlueprintCallable reflection bridge.
//
// This batch is intentionally read-only/plan-first. Generic invocation is
// fail-closed unless the reflected UFunction is public, BlueprintCallable, and
// passes the local read-only allowlist/safety policy.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "HAL/CriticalSection.h"
#include "HAL/UnrealMemory.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Misc/EngineVersion.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"
#include "UObject/Class.h"
#include "UObject/FieldIterator.h"
#include "UObject/TextProperty.h"
#include "UObject/UObjectIterator.h"

namespace UE::SOMOLMCP
{
namespace BlueprintCallableBridge
{
	struct FResolvedFunction
	{
		UClass* Class = nullptr;
		UFunction* Function = nullptr;
	};

	struct FPluginGate
	{
		FString ModuleName;
		FString PluginName;
		bool bPluginRequired = false;
		bool bPluginFound = false;
		bool bPluginEnabled = false;
		FString Status = TEXT("built_in_or_project_module");
	};

	static TArray<TSharedPtr<FJsonValue>> StringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Json;
		Json.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Json.Add(MakeShared<FJsonValueString>(Value));
		}
		return Json;
	}

	static FString CurrentEngineVersionString()
	{
		return FEngineVersion::Current().ToString();
	}

	static FString NormalizeName(FString Value)
	{
		Value.TrimStartAndEndInline();
		if (Value.StartsWith(TEXT("Class'")) && Value.EndsWith(TEXT("'")))
		{
			Value = Value.Mid(6, Value.Len() - 7);
		}
		return Value;
	}

	static FString ModuleNameForClass(const UClass* Class)
	{
		if (!Class)
		{
			return FString();
		}
		const FString PackageName = Class->GetOutermost() ? Class->GetOutermost()->GetName() : FString();
		if (PackageName.StartsWith(TEXT("/Script/")))
		{
			return PackageName.RightChop(8);
		}
		return PackageName;
	}

	static FString CategoryForFunction(const UFunction* Function)
	{
		if (!Function)
		{
			return FString();
		}
#if WITH_METADATA
		const FString Category = Function->GetMetaData(TEXT("Category"));
		return Category.IsEmpty() ? TEXT("Uncategorized") : Category;
#else
		return TEXT("metadata_unavailable");
#endif
	}

	static UClass* ResolveClass(const FString& RawClassName)
	{
		const FString ClassName = NormalizeName(RawClassName);
		if (ClassName.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassName))
		{
			return LoadedClass;
		}

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Candidate = *It;
			if (!Candidate)
			{
				continue;
			}

			const FString CandidateName = Candidate->GetName();
			const FString CandidatePath = Candidate->GetPathName();
			const FString ModuleQualifiedName = FString::Printf(TEXT("%s.%s"), *ModuleNameForClass(Candidate), *CandidateName);
			if (CandidateName == ClassName || CandidatePath == ClassName || ModuleQualifiedName == ClassName)
			{
				return Candidate;
			}
		}

		return nullptr;
	}

	static UFunction* ResolveFunction(UClass* Class, const FString& FunctionName)
	{
		if (!Class || FunctionName.IsEmpty())
		{
			return nullptr;
		}
		if (UFunction* Direct = Class->FindFunctionByName(FName(*FunctionName)))
		{
			return Direct;
		}
		for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			UFunction* Candidate = *It;
			if (Candidate && Candidate->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	static FResolvedFunction ResolveFunctionFromArgs(const TSharedRef<FJsonObject>& Arguments)
	{
		FString ClassName;
		FString FunctionName;
		Arguments->TryGetStringField(TEXT("class_name"), ClassName);
		Arguments->TryGetStringField(TEXT("function_name"), FunctionName);

		FResolvedFunction Resolved;
		Resolved.Class = ResolveClass(ClassName);
		Resolved.Function = ResolveFunction(Resolved.Class, FunctionName);
		return Resolved;
	}

	static FPluginGate BuildPluginGate(const FString& ModuleName)
	{
		FPluginGate Gate;
		Gate.ModuleName = ModuleName;
		if (ModuleName.IsEmpty())
		{
			Gate.Status = TEXT("module_unknown");
			return Gate;
		}

		const TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetDiscoveredPlugins();
		for (const TSharedRef<IPlugin>& Plugin : Plugins)
		{
			const FPluginDescriptor& Desc = Plugin->GetDescriptor();
			for (const FModuleDescriptor& Module : Desc.Modules)
			{
				if (Module.Name.ToString() == ModuleName)
				{
					Gate.PluginName = Plugin->GetName();
					Gate.bPluginRequired = true;
					Gate.bPluginFound = true;
					Gate.bPluginEnabled = Plugin->IsEnabled();
					Gate.Status = Gate.bPluginEnabled ? TEXT("plugin_enabled") : TEXT("plugin_present_not_enabled");
					return Gate;
				}
			}
		}

		FString ModulePath;
		if (ModuleExistsCompat(*ModuleName, &ModulePath))
		{
			Gate.Status = TEXT("module_exists_no_plugin_gate");
		}
		return Gate;
	}

	static TSharedRef<FJsonObject> PluginGateJson(const FPluginGate& Gate)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("module"), Gate.ModuleName);
		Obj->SetStringField(TEXT("status"), Gate.Status);
		Obj->SetBoolField(TEXT("plugin_required"), Gate.bPluginRequired);
		Obj->SetArrayField(TEXT("required_plugins"), Gate.PluginName.IsEmpty() ? TArray<TSharedPtr<FJsonValue>>() : StringArrayJson({Gate.PluginName}));
		if (!Gate.PluginName.IsEmpty())
		{
			Obj->SetStringField(TEXT("plugin"), Gate.PluginName);
			Obj->SetBoolField(TEXT("plugin_found"), Gate.bPluginFound);
			Obj->SetBoolField(TEXT("plugin_enabled"), Gate.bPluginEnabled);
		}
		return Obj;
	}

	static TSharedRef<FJsonObject> VersionJson()
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("min_engine_version"), TEXT("5.7.0"));
		Obj->SetStringField(TEXT("current_engine_version"), CurrentEngineVersionString());
		Obj->SetBoolField(TEXT("ue57_safe"), true);
		Obj->SetBoolField(TEXT("ue58_only"), false);
		return Obj;
	}

	static FString PropertyDirection(const FProperty* Property)
	{
		if (!Property)
		{
			return TEXT("unknown");
		}
		if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			return TEXT("return");
		}
		if (Property->HasAnyPropertyFlags(CPF_OutParm) && !Property->HasAnyPropertyFlags(CPF_ConstParm))
		{
			return TEXT("out");
		}
		return TEXT("in");
	}

	static FString PropertyJsonType(const FProperty* Property)
	{
		if (!Property)
		{
			return TEXT("unknown");
		}
		if (Property->IsA<FBoolProperty>())
		{
			return TEXT("boolean");
		}
		if (Property->IsA<FNumericProperty>())
		{
			return TEXT("number");
		}
		if (Property->IsA<FStrProperty>() || Property->IsA<FNameProperty>() || Property->IsA<FTextProperty>() || Property->IsA<FEnumProperty>())
		{
			return TEXT("string");
		}
		if (Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>())
		{
			return TEXT("array");
		}
		if (Property->IsA<FStructProperty>() || Property->IsA<FMapProperty>())
		{
			return TEXT("object");
		}
		if (Property->IsA<FObjectPropertyBase>() || Property->IsA<FClassProperty>() || Property->IsA<FSoftObjectProperty>() || Property->IsA<FSoftClassProperty>())
		{
			return TEXT("object_path");
		}
		return TEXT("string");
	}

	static bool IsSupportedInvokeProperty(const FProperty* Property)
	{
		return Property
			&& (Property->IsA<FBoolProperty>()
				|| Property->IsA<FNumericProperty>()
				|| Property->IsA<FStrProperty>()
				|| Property->IsA<FNameProperty>()
				|| Property->IsA<FTextProperty>()
				|| Property->IsA<FEnumProperty>()
				|| Property->IsA<FByteProperty>());
	}

	static TSharedRef<FJsonObject> PropertyJson(const FProperty* Property)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Property)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("name"), Property->GetName());
		Obj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		Obj->SetStringField(TEXT("json_type"), PropertyJsonType(Property));
		Obj->SetStringField(TEXT("direction"), PropertyDirection(Property));
		Obj->SetBoolField(TEXT("is_return"), Property->HasAnyPropertyFlags(CPF_ReturnParm));
		Obj->SetBoolField(TEXT("is_out"), Property->HasAnyPropertyFlags(CPF_OutParm));
		Obj->SetBoolField(TEXT("is_const"), Property->HasAnyPropertyFlags(CPF_ConstParm));
		Obj->SetBoolField(TEXT("invoke_supported"), IsSupportedInvokeProperty(Property));
		return Obj;
	}

	static void BuildParamArrays(const UFunction* Function, TArray<TSharedPtr<FJsonValue>>& OutParams, TArray<TSharedPtr<FJsonValue>>& OutReturns)
	{
		if (!Function)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}
			TSharedRef<FJsonObject> Param = PropertyJson(Property);
			if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				OutReturns.Add(MakeShared<FJsonValueObject>(Param));
			}
			else
			{
				OutParams.Add(MakeShared<FJsonValueObject>(Param));
			}
		}
	}

	static TArray<FString> DenyPrefixes()
	{
		return {
			TEXT("add"), TEXT("apply"), TEXT("attach"), TEXT("bind"), TEXT("clear"), TEXT("compile"), TEXT("connect"),
			TEXT("create"), TEXT("delete"), TEXT("destroy"), TEXT("duplicate"), TEXT("enable"), TEXT("disable"),
			TEXT("execute"), TEXT("generate"), TEXT("import"), TEXT("load"), TEXT("modify"), TEXT("move"),
			TEXT("open"), TEXT("play"), TEXT("remove"), TEXT("rename"), TEXT("reset"), TEXT("run"), TEXT("save"),
			TEXT("set"), TEXT("spawn"), TEXT("start"), TEXT("stop"), TEXT("toggle"), TEXT("write")
		};
	}

	static TArray<FString> ReadOnlyPrefixes()
	{
		return {
			TEXT("can"), TEXT("contains"), TEXT("find"), TEXT("get"), TEXT("has"), TEXT("is"), TEXT("k2_get"),
			TEXT("list"), TEXT("query"), TEXT("validate")
		};
	}

	static TArray<FString> ExplicitAllowlist()
	{
		return {
			TEXT("Actor.K2_GetActorLocation"),
			TEXT("Actor.K2_GetActorRotation"),
			TEXT("Actor.GetActorScale3D"),
			TEXT("SceneComponent.K2_GetComponentLocation"),
			TEXT("SceneComponent.K2_GetComponentRotation"),
			TEXT("SceneComponent.GetComponentScale"),
			TEXT("PrimitiveComponent.IsSimulatingPhysics")
		};
	}

	static FCriticalSection& DynamicAllowlistMutex()
	{
		static FCriticalSection Mutex;
		return Mutex;
	}

	static TSet<FString>& DynamicAllowlist()
	{
		static TSet<FString> Entries;
		return Entries;
	}

	static FString CallableKeyFor(UClass* Class, const UFunction* Function)
	{
		return FString::Printf(TEXT("%s.%s"), Class ? *Class->GetName() : TEXT(""), Function ? *Function->GetName() : TEXT(""));
	}

	static bool FunctionMatchesAnyClass(UClass* Class, const FString& ShortClassName)
	{
		for (UClass* Cursor = Class; Cursor; Cursor = Cursor->GetSuperClass())
		{
			if (Cursor->GetName() == ShortClassName)
			{
				return true;
			}
		}
		return false;
	}

	static bool MatchesExplicitAllowlist(UClass* Class, const UFunction* Function, FString& OutRule)
	{
		if (!Class || !Function)
		{
			return false;
		}
		for (const FString& Entry : ExplicitAllowlist())
		{
			FString ClassPart;
			FString FunctionPart;
			if (!Entry.Split(TEXT("."), &ClassPart, &FunctionPart))
			{
				continue;
			}
			if (FunctionMatchesAnyClass(Class, ClassPart) && Function->GetName() == FunctionPart)
			{
				OutRule = FString::Printf(TEXT("explicit:%s"), *Entry);
				return true;
			}
		}
		return false;
	}

	static bool MatchesDynamicAllowlist(UClass* Class, const UFunction* Function, FString& OutRule)
	{
		if (!Class || !Function)
		{
			return false;
		}
		const FString Key = CallableKeyFor(Class, Function);
		FScopeLock Lock(&DynamicAllowlistMutex());
		if (DynamicAllowlist().Contains(Key))
		{
			OutRule = FString::Printf(TEXT("runtime_allowlist:%s"), *Key);
			return true;
		}
		return false;
	}

	static TSharedRef<FJsonObject> SafetyJson(
		UClass* Class,
		const UFunction* Function,
		bool& bAllowed,
		FString& OutDenyCode,
		FString& OutDenyMessage,
		FString& OutRule)
	{
		bAllowed = false;
		OutDenyCode.Reset();
		OutDenyMessage.Reset();
		OutRule.Reset();

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("operation_class"), TEXT("read"));
		Obj->SetStringField(TEXT("safety_class"), TEXT("readonly"));
		Obj->SetBoolField(TEXT("generic_mutation_default_deny"), true);

		if (!Class)
		{
			OutDenyCode = TEXT("class_not_found");
			OutDenyMessage = TEXT("class_name did not resolve to a loaded UClass.");
		}
		else if (!Function)
		{
			OutDenyCode = TEXT("function_not_found");
			OutDenyMessage = TEXT("function_name did not resolve on class or superclasses.");
		}
		else if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
		{
			OutDenyCode = TEXT("not_blueprint_callable");
			OutDenyMessage = TEXT("Readonly invoke requires FUNC_BlueprintCallable.");
		}
		else if (!Function->HasAnyFunctionFlags(FUNC_Public))
		{
			OutDenyCode = TEXT("not_public");
			OutDenyMessage = TEXT("Readonly invoke requires FUNC_Public.");
		}
		else
		{
			const FString LowerName = Function->GetName().ToLower();
			bool bDeniedByPrefix = false;
			for (const FString& Prefix : DenyPrefixes())
			{
				if (LowerName.StartsWith(Prefix))
				{
					bDeniedByPrefix = true;
					OutDenyCode = TEXT("mutation_name_pattern");
					OutDenyMessage = FString::Printf(TEXT("Function name '%s' matches deny prefix '%s'."), *Function->GetName(), *Prefix);
					OutRule = FString::Printf(TEXT("deny_prefix:%s"), *Prefix);
					break;
				}
			}

			if (!bDeniedByPrefix && MatchesExplicitAllowlist(Class, Function, OutRule))
			{
				bAllowed = true;
			}
			else if (!bDeniedByPrefix && MatchesDynamicAllowlist(Class, Function, OutRule))
			{
				bAllowed = true;
			}
			else if (!bDeniedByPrefix)
			{
				bool bPrefixAllowed = false;
				for (const FString& Prefix : ReadOnlyPrefixes())
				{
					if (LowerName.StartsWith(Prefix))
					{
						bPrefixAllowed = true;
						OutRule = FString::Printf(TEXT("readonly_prefix:%s"), *Prefix);
						break;
					}
				}
				const bool bConstOrPure = Function->HasAnyFunctionFlags(FUNC_Const) || Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
				if (bPrefixAllowed && bConstOrPure)
				{
					bAllowed = true;
				}
				else
				{
					OutDenyCode = bPrefixAllowed ? TEXT("not_const_or_pure") : TEXT("not_allowlisted");
					OutDenyMessage = bPrefixAllowed
						? TEXT("Function name looks read-only, but UFunction is neither FUNC_Const nor FUNC_BlueprintPure.")
						: TEXT("Function is not matched by the curated read-only allowlist.");
				}
			}
		}

		Obj->SetBoolField(TEXT("readonly_invoke_allowed"), bAllowed);
		Obj->SetStringField(TEXT("allowlist_rule"), OutRule.IsEmpty() ? TEXT("none") : OutRule);
		Obj->SetStringField(TEXT("deny_code"), OutDenyCode);
		Obj->SetStringField(TEXT("deny_message"), OutDenyMessage);
		return Obj;
	}

	static TSharedRef<FJsonObject> FunctionJson(UClass* Class, UFunction* Function, const bool bIncludeParams)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		const FString ModuleName = ModuleNameForClass(Class);
		const FPluginGate Gate = BuildPluginGate(ModuleName);

		Obj->SetStringField(TEXT("class"), Class ? Class->GetName() : FString());
		Obj->SetStringField(TEXT("class_path"), Class ? Class->GetPathName() : FString());
		Obj->SetStringField(TEXT("function"), Function ? Function->GetName() : FString());
		Obj->SetStringField(TEXT("module"), ModuleName);
		Obj->SetStringField(TEXT("category"), CategoryForFunction(Function));
		Obj->SetBoolField(TEXT("blueprint_callable"), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		Obj->SetBoolField(TEXT("public"), Function && Function->HasAnyFunctionFlags(FUNC_Public));
		Obj->SetBoolField(TEXT("const"), Function && Function->HasAnyFunctionFlags(FUNC_Const));
		Obj->SetBoolField(TEXT("blueprint_pure"), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
		Obj->SetBoolField(TEXT("static"), Function && Function->HasAnyFunctionFlags(FUNC_Static));

		TArray<TSharedPtr<FJsonValue>> Params;
		TArray<TSharedPtr<FJsonValue>> Returns;
		if (bIncludeParams)
		{
			BuildParamArrays(Function, Params, Returns);
		}
		Obj->SetArrayField(TEXT("params"), Params);
		Obj->SetArrayField(TEXT("return"), Returns);

		bool bAllowed = false;
		FString DenyCode;
		FString DenyMessage;
		FString Rule;
		Obj->SetObjectField(TEXT("safety"), SafetyJson(Class, Function, bAllowed, DenyCode, DenyMessage, Rule));
		Obj->SetObjectField(TEXT("version"), VersionJson());
		Obj->SetObjectField(TEXT("plugin_gate"), PluginGateJson(Gate));
		return Obj;
	}

	static TSharedRef<FJsonObject> CommonFilterSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("class_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional case-insensitive class name/path substring."))},
			{TEXT("module_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional module name, e.g. Engine, UMG, PCG."))},
			{TEXT("category_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional Blueprint metadata category substring."))},
			{TEXT("max_functions"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum functions to return; default 200, cap 2000."))},
			{TEXT("include_params"), FSololmcpSchemaBuilder::Boolean(TEXT("Include reflected params/return arrays; default true."))}
		});
	}

	static bool RunInventory(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString&)
	{
		FString ClassFilter;
		FString ModuleFilter;
		FString CategoryFilter;
		Arguments->TryGetStringField(TEXT("class_filter"), ClassFilter);
		Arguments->TryGetStringField(TEXT("module_filter"), ModuleFilter);
		Arguments->TryGetStringField(TEXT("category_filter"), CategoryFilter);
		ClassFilter = ClassFilter.ToLower();
		ModuleFilter = ModuleFilter.ToLower();
		CategoryFilter = CategoryFilter.ToLower();

		int32 MaxFunctions = 200;
		Arguments->TryGetNumberField(TEXT("max_functions"), MaxFunctions);
		MaxFunctions = FMath::Clamp(MaxFunctions <= 0 ? 200 : MaxFunctions, 1, 2000);

		bool bIncludeParams = true;
		Arguments->TryGetBoolField(TEXT("include_params"), bIncludeParams);

		TArray<TSharedPtr<FJsonValue>> Functions;
		int32 ScannedClasses = 0;
		int32 ScannedCallableFunctions = 0;
		for (TObjectIterator<UClass> It; It && Functions.Num() < MaxFunctions; ++It)
		{
			UClass* Class = *It;
			if (!Class)
			{
				continue;
			}
			++ScannedClasses;
			const FString ModuleName = ModuleNameForClass(Class);
			const FString ClassNeedle = FString::Printf(TEXT("%s %s"), *Class->GetName(), *Class->GetPathName()).ToLower();
			if (!ClassFilter.IsEmpty() && !ClassNeedle.Contains(ClassFilter))
			{
				continue;
			}
			if (!ModuleFilter.IsEmpty() && !ModuleName.ToLower().Contains(ModuleFilter))
			{
				continue;
			}

			for (TFieldIterator<UFunction> FnIt(Class, EFieldIteratorFlags::IncludeSuper); FnIt && Functions.Num() < MaxFunctions; ++FnIt)
			{
				UFunction* Function = *FnIt;
				if (!Function || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
				{
					continue;
				}
				++ScannedCallableFunctions;
				const FString Category = CategoryForFunction(Function);
				if (!CategoryFilter.IsEmpty() && !Category.ToLower().Contains(CategoryFilter))
				{
					continue;
				}
				Functions.Add(MakeShared<FJsonValueObject>(FunctionJson(Class, Function, bIncludeParams)));
			}
		}

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetStringField(TEXT("tool_name"), TEXT("mcp_blueprint_callable_inventory"));
		OutStructured->SetNumberField(TEXT("count"), Functions.Num());
		OutStructured->SetNumberField(TEXT("scanned_classes"), ScannedClasses);
		OutStructured->SetNumberField(TEXT("scanned_blueprint_callable_functions"), ScannedCallableFunctions);
		OutStructured->SetArrayField(TEXT("functions"), Functions);
		OutStructured->SetObjectField(TEXT("version"), VersionJson());
		OutSummary = FString::Printf(TEXT("Returned %d BlueprintCallable functions."), Functions.Num());
		return true;
	}

	static bool RunSchema(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		const FResolvedFunction Resolved = ResolveFunctionFromArgs(Arguments);
		if (!Resolved.Class || !Resolved.Function)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), Resolved.Class ? TEXT("function_not_found") : TEXT("class_not_found"));
			OutStructured->SetObjectField(TEXT("version"), VersionJson());
			OutError = TEXT("BlueprintCallable schema target did not resolve.");
			return false;
		}

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetStringField(TEXT("tool_name"), TEXT("mcp_blueprint_callable_schema"));
		OutStructured->SetObjectField(TEXT("callable"), FunctionJson(Resolved.Class, Resolved.Function, true));
		OutSummary = FString::Printf(TEXT("Reflected schema for %s.%s."), *Resolved.Class->GetName(), *Resolved.Function->GetName());
		return true;
	}

	static TSharedRef<FJsonObject> AllowlistPolicyJson()
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("policy_version"), TEXT("blueprint_callable_readonly_v1"));
		Obj->SetStringField(TEXT("default"), TEXT("deny"));
		Obj->SetBoolField(TEXT("requires_blueprint_callable"), true);
		Obj->SetBoolField(TEXT("requires_public"), true);
		Obj->SetBoolField(TEXT("requires_const_or_blueprint_pure_for_prefix_rules"), true);
		Obj->SetArrayField(TEXT("readonly_prefix_allowlist"), StringArrayJson(ReadOnlyPrefixes()));
		Obj->SetArrayField(TEXT("mutation_prefix_denylist"), StringArrayJson(DenyPrefixes()));
		Obj->SetArrayField(TEXT("explicit_function_allowlist"), StringArrayJson(ExplicitAllowlist()));
		TArray<FString> RuntimeEntries;
		{
			FScopeLock Lock(&DynamicAllowlistMutex());
			for (const FString& Entry : DynamicAllowlist())
			{
				RuntimeEntries.Add(Entry);
			}
		}
		RuntimeEntries.Sort();
		Obj->SetArrayField(TEXT("runtime_function_allowlist"), StringArrayJson(RuntimeEntries));
		Obj->SetObjectField(TEXT("version"), VersionJson());
		return Obj;
	}

	static bool RunAllowlistGet(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>&,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString&)
	{
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetStringField(TEXT("tool_name"), TEXT("mcp_callable_allowlist_get"));
		OutStructured->SetObjectField(TEXT("allowlist"), AllowlistPolicyJson());
		OutSummary = TEXT("Returned BlueprintCallable readonly allowlist policy.");
		return true;
	}

	static bool RunDenyReason(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString&)
	{
		const FResolvedFunction Resolved = ResolveFunctionFromArgs(Arguments);
		bool bAllowed = false;
		FString DenyCode;
		FString DenyMessage;
		FString Rule;
		TSharedRef<FJsonObject> Safety = SafetyJson(Resolved.Class, Resolved.Function, bAllowed, DenyCode, DenyMessage, Rule);

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetStringField(TEXT("tool_name"), TEXT("mcp_callable_deny_reason"));
		OutStructured->SetStringField(TEXT("class"), Resolved.Class ? Resolved.Class->GetName() : FString());
		OutStructured->SetStringField(TEXT("function"), Resolved.Function ? Resolved.Function->GetName() : FString());
		OutStructured->SetBoolField(TEXT("allowed"), bAllowed);
		OutStructured->SetObjectField(TEXT("safety"), Safety);
		OutStructured->SetObjectField(TEXT("version"), VersionJson());
		OutSummary = bAllowed ? TEXT("Callable is allowed by readonly policy.") : FString::Printf(TEXT("Callable denied: %s."), *DenyCode);
		return true;
	}

	static bool RunAllowlistSet(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		const FResolvedFunction Resolved = ResolveFunctionFromArgs(Arguments);
		bool bDryRun = true;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
		FString Action = TEXT("add");
		Arguments->TryGetStringField(TEXT("action"), Action);
		Action = Action.ToLower();

		bool bAllowedBefore = false;
		FString DenyCode;
		FString DenyMessage;
		FString Rule;
		TSharedRef<FJsonObject> Safety = SafetyJson(Resolved.Class, Resolved.Function, bAllowedBefore, DenyCode, DenyMessage, Rule);

		const FString Key = CallableKeyFor(Resolved.Class, Resolved.Function);
		const bool bResolved = Resolved.Class && Resolved.Function;
		const bool bShapeOk = bResolved
			&& Resolved.Function->HasAnyFunctionFlags(FUNC_BlueprintCallable)
			&& Resolved.Function->HasAnyFunctionFlags(FUNC_Public)
			&& (Resolved.Function->HasAnyFunctionFlags(FUNC_Const) || Resolved.Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
		bool bHasUnsupportedParam = false;
		if (bResolved)
		{
			for (TFieldIterator<FProperty> It(Resolved.Function); It; ++It)
			{
				const FProperty* Property = *It;
				if (Property && Property->HasAnyPropertyFlags(CPF_Parm) && !Property->HasAnyPropertyFlags(CPF_ReturnParm) && !IsSupportedInvokeProperty(Property))
				{
					bHasUnsupportedParam = true;
					break;
				}
			}
		}

		OutStructured->SetStringField(TEXT("tool_name"), TEXT("mcp_callable_allowlist_set"));
		OutStructured->SetBoolField(TEXT("read_only"), bDryRun);
		OutStructured->SetObjectField(TEXT("version"), VersionJson());
		OutStructured->SetObjectField(TEXT("safety_before"), Safety);
		OutStructured->SetStringField(TEXT("action"), Action);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetStringField(TEXT("allowlist_key"), Key);

		if (!bResolved)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), Resolved.Class ? TEXT("function_not_found") : TEXT("class_not_found"));
			OutError = TEXT("Allowlist target did not resolve.");
			return false;
		}
		if (Action != TEXT("add") && Action != TEXT("remove"))
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("invalid_action"));
			OutError = TEXT("action must be add or remove.");
			return false;
		}
		if (Action == TEXT("add") && (!bShapeOk || bHasUnsupportedParam || DenyCode == TEXT("mutation_name_pattern")))
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("denied"));
			OutStructured->SetStringField(TEXT("deny_code"), bHasUnsupportedParam ? TEXT("unsupported_parameter_type") : (DenyCode.IsEmpty() ? TEXT("not_readonly_shape") : DenyCode));
			OutStructured->SetStringField(TEXT("deny_message"), TEXT("Runtime allowlist can only add public BlueprintCallable const/pure functions with primitive invoke-supported parameters, and cannot override mutation-name deny prefixes."));
			OutSummary = TEXT("Runtime allowlist update denied.");
			OutError = OutSummary;
			return false;
		}

		if (!bDryRun)
		{
			FScopeLock Lock(&DynamicAllowlistMutex());
			if (Action == TEXT("add"))
			{
				DynamicAllowlist().Add(Key);
			}
			else
			{
				DynamicAllowlist().Remove(Key);
			}
		}

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetStringField(TEXT("status"), bDryRun ? TEXT("dry_run_ok") : TEXT("updated"));
		OutStructured->SetObjectField(TEXT("allowlist"), AllowlistPolicyJson());
		OutSummary = bDryRun
			? FString::Printf(TEXT("Runtime allowlist dry-run accepted for %s."), *Key)
			: FString::Printf(TEXT("Runtime allowlist %s applied for %s."), *Action, *Key);
		return true;
	}

	static TSharedPtr<FJsonObject> ArgsObject(const TSharedRef<FJsonObject>& Arguments)
	{
		const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
		if (Arguments->TryGetObjectField(TEXT("args"), ArgsPtr) && ArgsPtr && ArgsPtr->IsValid())
		{
			return *ArgsPtr;
		}
		return MakeShared<FJsonObject>();
	}

	static bool ApplyJsonToProperty(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
	{
		if (!Property || !JsonValue.IsValid() || JsonValue->IsNull())
		{
			return true;
		}

		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			BoolProperty->SetPropertyValue(ValuePtr, JsonValue->AsBool());
			return true;
		}
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsInteger())
			{
				NumericProperty->SetIntPropertyValue(ValuePtr, static_cast<int64>(JsonValue->AsNumber()));
			}
			else
			{
				NumericProperty->SetFloatingPointPropertyValue(ValuePtr, JsonValue->AsNumber());
			}
			return true;
		}
		if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			StrProperty->SetPropertyValue(ValuePtr, JsonValue->AsString());
			return true;
		}
		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			NameProperty->SetPropertyValue(ValuePtr, FName(*JsonValue->AsString()));
			return true;
		}
		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			TextProperty->SetPropertyValue(ValuePtr, FText::FromString(JsonValue->AsString()));
			return true;
		}
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 EnumValue = EnumProperty->GetEnum()->GetValueByNameString(JsonValue->AsString());
			if (EnumValue == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Invalid enum value '%s' for %s."), *JsonValue->AsString(), *Property->GetName());
				return false;
			}
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumValue);
			return true;
		}
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			ByteProperty->SetIntPropertyValue(ValuePtr, static_cast<int64>(JsonValue->AsNumber()));
			return true;
		}

		OutError = FString::Printf(TEXT("Parameter '%s' type '%s' is not supported by readonly generic invoke."), *Property->GetName(), *Property->GetCPPType());
		return false;
	}

	static TSharedPtr<FJsonValue> ExportPropertyValue(FProperty* Property, const void* ValuePtr)
	{
		if (!Property)
		{
			return MakeShared<FJsonValueNull>();
		}
		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
		}
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			return MakeShared<FJsonValueNumber>(NumericProperty->IsInteger()
				? static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr))
				: NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
		}
		if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			return MakeShared<FJsonValueString>(StrProperty->GetPropertyValue(ValuePtr));
		}
		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString());
		}
		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString());
		}

		FString Exported;
		Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(Exported);
	}

	static UObject* ResolveInvokeTarget(UClass* Class, UFunction* Function, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
	{
		FString TargetObjectPath;
		Arguments->TryGetStringField(TEXT("target_object_path"), TargetObjectPath);
		TargetObjectPath.TrimStartAndEndInline();

		if (!TargetObjectPath.IsEmpty())
		{
			UObject* Target = LoadObject<UObject>(nullptr, *TargetObjectPath);
			if (!Target)
			{
				OutError = FString::Printf(TEXT("target_object_path '%s' did not load."), *TargetObjectPath);
				return nullptr;
			}
			if (!Target->IsA(Class))
			{
				OutError = FString::Printf(TEXT("target_object_path '%s' is not a %s."), *TargetObjectPath, *Class->GetName());
				return nullptr;
			}
			return Target;
		}

		if (Function->HasAnyFunctionFlags(FUNC_Static) || Class->IsChildOf<UBlueprintFunctionLibrary>())
		{
			return Class->GetDefaultObject();
		}

		bool bAllowCdo = false;
		Arguments->TryGetBoolField(TEXT("allow_cdo"), bAllowCdo);
		if (bAllowCdo)
		{
			return Class->GetDefaultObject();
		}

		OutError = TEXT("Readonly invoke requires target_object_path for instance functions unless allow_cdo=true.");
		return nullptr;
	}

	static bool RunInvokeReadonly(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		const FResolvedFunction Resolved = ResolveFunctionFromArgs(Arguments);
		bool bAllowed = false;
		FString DenyCode;
		FString DenyMessage;
		FString Rule;
		TSharedRef<FJsonObject> Safety = SafetyJson(Resolved.Class, Resolved.Function, bAllowed, DenyCode, DenyMessage, Rule);

		OutStructured->SetStringField(TEXT("tool_name"), TEXT("mcp_blueprint_callable_invoke_readonly"));
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetObjectField(TEXT("safety"), Safety);
		OutStructured->SetObjectField(TEXT("version"), VersionJson());

		if (!bAllowed)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("denied"));
			OutStructured->SetStringField(TEXT("deny_code"), DenyCode);
			OutStructured->SetStringField(TEXT("deny_message"), DenyMessage);
			OutSummary = FString::Printf(TEXT("Readonly invoke denied: %s."), *DenyCode);
			OutError = OutSummary;
			return false;
		}

		for (TFieldIterator<FProperty> It(Resolved.Function); It; ++It)
		{
			FProperty* Property = *It;
			if (Property && Property->HasAnyPropertyFlags(CPF_Parm) && !Property->HasAnyPropertyFlags(CPF_ReturnParm) && !IsSupportedInvokeProperty(Property))
			{
				OutStructured->SetBoolField(TEXT("success"), false);
				OutStructured->SetStringField(TEXT("status"), TEXT("denied"));
				OutStructured->SetStringField(TEXT("deny_code"), TEXT("unsupported_parameter_type"));
				OutStructured->SetStringField(TEXT("deny_message"), FString::Printf(TEXT("Parameter '%s' type '%s' is unsupported by generic readonly invoke."), *Property->GetName(), *Property->GetCPPType()));
				OutSummary = TEXT("Readonly invoke denied: unsupported parameter type.");
				OutError = OutSummary;
				return false;
			}
		}

		FString TargetError;
		UObject* Target = ResolveInvokeTarget(Resolved.Class, Resolved.Function, Arguments, TargetError);
		if (!Target)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("denied"));
			OutStructured->SetStringField(TEXT("deny_code"), TEXT("target_required"));
			OutStructured->SetStringField(TEXT("deny_message"), TargetError);
			OutSummary = TEXT("Readonly invoke denied: target required.");
			OutError = TargetError;
			return false;
		}

		uint8* ParamsBuffer = static_cast<uint8*>(FMemory_Alloca(Resolved.Function->ParmsSize));
		Resolved.Function->InitializeStruct(ParamsBuffer);

		bool bParamsOk = true;
		FString ParamError;
		TSharedPtr<FJsonObject> Args = ArgsObject(Arguments);
		for (TFieldIterator<FProperty> It(Resolved.Function); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Parm) || Property->HasAnyPropertyFlags(CPF_ReturnParm) || Property->HasAnyPropertyFlags(CPF_OutParm))
			{
				continue;
			}
			const TSharedPtr<FJsonValue> JsonValue = Args->TryGetField(Property->GetName());
			if (!ApplyJsonToProperty(Property, Property->ContainerPtrToValuePtr<void>(ParamsBuffer), JsonValue, ParamError))
			{
				bParamsOk = false;
				break;
			}
		}

		if (!bParamsOk)
		{
			Resolved.Function->DestroyStruct(ParamsBuffer);
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("denied"));
			OutStructured->SetStringField(TEXT("deny_code"), TEXT("argument_conversion_failed"));
			OutStructured->SetStringField(TEXT("deny_message"), ParamError);
			OutSummary = TEXT("Readonly invoke denied: argument conversion failed.");
			OutError = ParamError;
			return false;
		}

		Target->ProcessEvent(Resolved.Function, ParamsBuffer);

		TArray<TSharedPtr<FJsonValue>> ReturnValues;
		for (TFieldIterator<FProperty> It(Resolved.Function); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Parm) || (!Property->HasAnyPropertyFlags(CPF_ReturnParm) && !Property->HasAnyPropertyFlags(CPF_OutParm)))
			{
				continue;
			}
			TSharedRef<FJsonObject> ValueObj = PropertyJson(Property);
			ValueObj->SetField(TEXT("value"), ExportPropertyValue(Property, Property->ContainerPtrToValuePtr<void>(ParamsBuffer)));
			ReturnValues.Add(MakeShared<FJsonValueObject>(ValueObj));
		}

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetStringField(TEXT("status"), TEXT("invoked_readonly"));
		OutStructured->SetStringField(TEXT("target"), Target->GetPathName());
		OutStructured->SetObjectField(TEXT("callable"), FunctionJson(Resolved.Class, Resolved.Function, true));
		OutStructured->SetArrayField(TEXT("results"), ReturnValues);
		OutSummary = FString::Printf(TEXT("Invoked readonly callable %s.%s."), *Resolved.Class->GetName(), *Resolved.Function->GetName());

		Resolved.Function->DestroyStruct(ParamsBuffer);
		return true;
	}

	static FString SanitizeToolName(FString Value)
	{
		Value = Value.ToLower();
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			if (!FChar::IsAlnum(Value[Index]))
			{
				Value[Index] = TEXT('_');
			}
		}
		return Value;
	}

	static bool RunWrapperPlan(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		const FResolvedFunction Resolved = ResolveFunctionFromArgs(Arguments);
		if (!Resolved.Class || !Resolved.Function)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), Resolved.Class ? TEXT("function_not_found") : TEXT("class_not_found"));
			OutStructured->SetObjectField(TEXT("version"), VersionJson());
			OutError = TEXT("Wrapper plan target did not resolve.");
			return false;
		}

		bool bAllowed = false;
		FString DenyCode;
		FString DenyMessage;
		FString Rule;
		TSharedRef<FJsonObject> Safety = SafetyJson(Resolved.Class, Resolved.Function, bAllowed, DenyCode, DenyMessage, Rule);

		TArray<TSharedPtr<FJsonValue>> Steps = StringArrayJson({
			TEXT("Create a named SOMOLMCP wrapper instead of relying on broad generic mutation."),
			TEXT("Keep default mode dry_run/read_only unless explicit target binding and receipt gates are present."),
			TEXT("Validate BlueprintCallable + FUNC_Public reflection schema at runtime."),
			TEXT("Attach plugin/module gate, version gate, pre-readback, post-readback, and failure route fields."),
			TEXT("For write wrappers, require target lock, transaction/rollback note, compile or validate receipt, and screenshot/preview proof where relevant.")
		});

		TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("suggested_tool_name"), FString::Printf(TEXT("bp_callable_%s_%s"), *SanitizeToolName(Resolved.Class->GetName()), *SanitizeToolName(Resolved.Function->GetName())));
		Plan->SetStringField(TEXT("wrapper_mode"), bAllowed ? TEXT("readonly_wrapper") : TEXT("named_wrapper_required_before_invoke"));
		Plan->SetArrayField(TEXT("implementation_steps"), Steps);
		Plan->SetArrayField(TEXT("receipt_requirements"), StringArrayJson({
			TEXT("target asset/object/project binding"),
			TEXT("pre-call schema and safety snapshot"),
			TEXT("post-call readback"),
			TEXT("plugin/module/version gate"),
			TEXT("failure route and structured deny reason")
		}));

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetStringField(TEXT("tool_name"), TEXT("mcp_blueprint_callable_wrapper_plan"));
		OutStructured->SetObjectField(TEXT("callable"), FunctionJson(Resolved.Class, Resolved.Function, true));
		OutStructured->SetObjectField(TEXT("safety"), Safety);
		OutStructured->SetObjectField(TEXT("plan"), Plan);
		OutStructured->SetObjectField(TEXT("version"), VersionJson());
		OutSummary = FString::Printf(TEXT("Built wrapper plan for %s.%s."), *Resolved.Class->GetName(), *Resolved.Function->GetName());
		return true;
	}

	static bool HasAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Fields)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const FString& Field : Fields)
		{
			if (Obj->HasField(Field))
			{
				return true;
			}
		}
		return false;
	}

	static bool RunReceiptValidate(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString&)
	{
		const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr;
		const bool bHasReceipt = Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) && ReceiptPtr && ReceiptPtr->IsValid();
		TSharedPtr<FJsonObject> Receipt = bHasReceipt ? *ReceiptPtr : MakeShared<FJsonObject>();

		FString OperationClass = TEXT("read");
		Arguments->TryGetStringField(TEXT("operation_class"), OperationClass);
		FString ReceiptOperation;
		if (Receipt->TryGetStringField(TEXT("operation_class"), ReceiptOperation) && !ReceiptOperation.IsEmpty())
		{
			OperationClass = ReceiptOperation;
		}
		OperationClass = OperationClass.ToLower();
		const bool bMutation = OperationClass != TEXT("read") && OperationClass != TEXT("readonly") && OperationClass != TEXT("read_only");

		TArray<FString> Missing;
		auto Require = [&Missing, Receipt](const FString& Requirement, const TArray<FString>& Fields)
		{
			if (!HasAnyField(Receipt, Fields))
			{
				Missing.Add(Requirement);
			}
		};

		if (!bHasReceipt)
		{
			Missing.Add(TEXT("receipt object"));
		}
		Require(TEXT("callable identity"), {TEXT("callable"), TEXT("class_name"), TEXT("function_name"), TEXT("tool_name")});
		Require(TEXT("target binding"), {TEXT("target"), TEXT("target_asset"), TEXT("target_object_path"), TEXT("project_path"), TEXT("project")});
		Require(TEXT("schema or preflight snapshot"), {TEXT("schema"), TEXT("preflight"), TEXT("pre_call_schema"), TEXT("safety")});
		Require(TEXT("version/plugin/module gate"), {TEXT("version"), TEXT("plugin_gate"), TEXT("module_gate"), TEXT("gate")});
		Require(TEXT("post-call readback or result"), {TEXT("post_readback"), TEXT("readback"), TEXT("result"), TEXT("results")});
		Require(TEXT("failure route or status"), {TEXT("failure_route"), TEXT("deny_code"), TEXT("status"), TEXT("error")});
		if (bMutation)
		{
			Require(TEXT("resource lock / transaction / rollback evidence"), {TEXT("resource_lock"), TEXT("transaction"), TEXT("rollback"), TEXT("rollback_note")});
			Require(TEXT("compile/validate/QA evidence"), {TEXT("compile"), TEXT("validation"), TEXT("validate"), TEXT("qa"), TEXT("qa_receipt")});
		}

		TArray<TSharedPtr<FJsonValue>> MissingJson = StringArrayJson(Missing);
		OutStructured->SetStringField(TEXT("tool_name"), TEXT("mcp_blueprint_callable_receipt_validate"));
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetBoolField(TEXT("passed"), Missing.IsEmpty());
		OutStructured->SetStringField(TEXT("status"), Missing.IsEmpty() ? TEXT("passed") : TEXT("missing_required_evidence"));
		OutStructured->SetStringField(TEXT("operation_class"), OperationClass);
		OutStructured->SetArrayField(TEXT("missing_requirements"), MissingJson);
		OutStructured->SetObjectField(TEXT("version"), VersionJson());
		OutSummary = Missing.IsEmpty()
			? TEXT("BlueprintCallable receipt passed required evidence gate.")
			: FString::Printf(TEXT("BlueprintCallable receipt missing %d required evidence item(s)."), Missing.Num());
		return true;
	}

	static TSharedRef<FJsonObject> ClassFunctionSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("class_name"), FSololmcpSchemaBuilder::String(TEXT("UClass name or path, e.g. /Script/Engine.Actor or Actor."))},
			{TEXT("function_name"), FSololmcpSchemaBuilder::String(TEXT("UFunction name."))}
		}, {TEXT("class_name"), TEXT("function_name")});
	}
}

	void RegisterBlueprintCallableBridgeTools(FSololmcpToolRegistry& Registry)
	{
		using namespace BlueprintCallableBridge;

		FSololmcpToolDefinition Inventory;
		Inventory.Name = TEXT("mcp_blueprint_callable_inventory");
		Inventory.Description = TEXT("Read-only UE 5.7-safe inventory of loaded BlueprintCallable UFunctions with schema, safety, version, and plugin gate metadata.");
		Inventory.InputSchema = CommonFilterSchema();
		Inventory.Execute = RunInventory;
		Inventory.CacheTtlSeconds = 30;
		Registry.Register(Inventory);

		FSololmcpToolDefinition Schema;
		Schema.Name = TEXT("mcp_blueprint_callable_schema");
		Schema.Description = TEXT("Reflect one BlueprintCallable UFunction schema including params, return values, safety, UE version, module, category, and plugin gate.");
		Schema.InputSchema = ClassFunctionSchema();
		Schema.Execute = RunSchema;
		Schema.CacheTtlSeconds = 60;
		Registry.Register(Schema);

		FSololmcpToolDefinition Invoke;
		Invoke.Name = TEXT("mcp_blueprint_callable_invoke_readonly");
		Invoke.Description = TEXT("Fail-closed generic readonly BlueprintCallable invoke. Allows only public BlueprintCallable functions that pass the local readonly allowlist/safety policy.");
		Invoke.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("class_name"), FSololmcpSchemaBuilder::String(TEXT("UClass name or path."))},
			{TEXT("function_name"), FSololmcpSchemaBuilder::String(TEXT("UFunction name."))},
			{TEXT("target_object_path"), FSololmcpSchemaBuilder::String(TEXT("Optional object path for instance readonly calls."))},
			{TEXT("allow_cdo"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow invoking on class default object for instance functions; default false."))},
			{TEXT("args"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Primitive JSON args keyed by parameter name."))}
		}, {TEXT("class_name"), TEXT("function_name")});
		Invoke.Execute = RunInvokeReadonly;
		Invoke.CacheTtlSeconds = 0;
		Registry.Register(Invoke);

		FSololmcpToolDefinition WrapperPlan;
		WrapperPlan.Name = TEXT("mcp_blueprint_callable_wrapper_plan");
		WrapperPlan.Description = TEXT("Plan a named SOMOLMCP wrapper for a BlueprintCallable UFunction with explicit safety, version, plugin gate, and receipt requirements.");
		WrapperPlan.InputSchema = ClassFunctionSchema();
		WrapperPlan.Execute = RunWrapperPlan;
		WrapperPlan.CacheTtlSeconds = 30;
		Registry.Register(WrapperPlan);

		FSololmcpToolDefinition WrapperPlanAlias;
		WrapperPlanAlias.Name = TEXT("mcp_callable_wrapper_plan");
		WrapperPlanAlias.Description = TEXT("Compatibility alias for mcp_blueprint_callable_wrapper_plan.");
		WrapperPlanAlias.InputSchema = ClassFunctionSchema();
		WrapperPlanAlias.Execute = RunWrapperPlan;
		WrapperPlanAlias.CacheTtlSeconds = 30;
		Registry.Register(WrapperPlanAlias);

		FSololmcpToolDefinition Allowlist;
		Allowlist.Name = TEXT("mcp_callable_allowlist_get");
		Allowlist.Description = TEXT("Return the local fail-closed readonly allowlist/denylist policy used by generic BlueprintCallable invoke.");
		Allowlist.InputSchema = FSololmcpSchemaBuilder::Object({});
		Allowlist.Execute = RunAllowlistGet;
		Allowlist.CacheTtlSeconds = 60;
		Registry.Register(Allowlist);

		FSololmcpToolDefinition AllowlistSet;
		AllowlistSet.Name = TEXT("mcp_callable_allowlist_set");
		AllowlistSet.Description = TEXT("Add/remove a runtime readonly allowlist override for a public BlueprintCallable const/pure function; mutation-name patterns remain denied.");
		AllowlistSet.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("class_name"), FSololmcpSchemaBuilder::String(TEXT("UClass name or path."))},
			{TEXT("function_name"), FSololmcpSchemaBuilder::String(TEXT("UFunction name."))},
			{TEXT("action"), FSololmcpSchemaBuilder::String(TEXT("add or remove."), {TEXT("add"), TEXT("remove")})},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Validate the update without applying it; default true."))}
		}, {TEXT("class_name"), TEXT("function_name")});
		AllowlistSet.Execute = RunAllowlistSet;
		AllowlistSet.CacheTtlSeconds = 0;
		Registry.Register(AllowlistSet);

		FSololmcpToolDefinition DenyReason;
		DenyReason.Name = TEXT("mcp_callable_deny_reason");
		DenyReason.Description = TEXT("Explain whether a BlueprintCallable target would be allowed or denied by the readonly invoke policy, with structured deny reason.");
		DenyReason.InputSchema = ClassFunctionSchema();
		DenyReason.Execute = RunDenyReason;
		DenyReason.CacheTtlSeconds = 30;
		Registry.Register(DenyReason);

		FSololmcpToolDefinition ReceiptValidate;
		ReceiptValidate.Name = TEXT("mcp_blueprint_callable_receipt_validate");
		ReceiptValidate.Description = TEXT("Validate that a BlueprintCallable wrapper/invoke receipt contains target binding, gate, readback, failure route, and mutation QA evidence when needed.");
		ReceiptValidate.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Receipt object to validate."))},
			{TEXT("operation_class"), FSololmcpSchemaBuilder::String(TEXT("read, readonly, asset_write, level_write, runtime_state, or similar."))}
		});
		ReceiptValidate.Execute = RunReceiptValidate;
		ReceiptValidate.CacheTtlSeconds = 0;
		Registry.Register(ReceiptValidate);
	}
}
