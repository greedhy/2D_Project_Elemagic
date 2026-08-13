// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpUE58ToolsetTools.cpp
// ----------------------------------------------------------------------------
// UE 5.8 ToolsetRegistry / Toolsets probes and wrapper planning.
//
// This file intentionally avoids including UE 5.8-only ToolsetRegistry headers.
// All 5.8 surfaces are discovered through engine version gates, plugin
// descriptors, UObject reflection, and source scanning. MCPClientToolset is
// explicitly excluded and never connected.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP
{
namespace UE58ToolsetTools
{
	struct FToolsetCallable
	{
		FString Source;
		FString Plugin;
		FString Module;
		FString Class;
		FString Function;
		FString Category;
		FString ReturnType;
		FString FilePath;
		int32 Line = 0;
		TArray<TPair<FString, FString>> Params;
	};

	static FString CurrentEngineVersionString()
	{
		return FEngineVersion::Current().ToString();
	}

	static bool IsUE58OrLater()
	{
		const FEngineVersion Current = FEngineVersion::Current();
		if (Current.GetMajor() != 5)
		{
			return Current.GetMajor() > 5;
		}
		return Current.GetMinor() >= 8;
	}

	static FString ToolsetsDir()
	{
		return FPaths::Combine(FPaths::EnginePluginsDir(), TEXT("Experimental"), TEXT("Toolsets"));
	}

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

	static FString NormalizePathForScan(const FString& Path)
	{
		FString Normalized = Path;
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Normalized;
	}

	static bool IsExcludedToolsetPath(const FString& Path)
	{
		const FString Normalized = NormalizePathForScan(Path);
		return Normalized.Contains(TEXT("/MCPClientToolset/")) ||
			Normalized.EndsWith(TEXT("/MCPClientToolset.uplugin")) ||
			Normalized.Contains(TEXT("/Intermediate/"));
	}

	static FString ExtractPathPartAfterToolsets(const FString& Path, const int32 PartIndex)
	{
		const FString Normalized = NormalizePathForScan(Path);
		const FString Needle = TEXT("/Toolsets/");
		const int32 Start = Normalized.Find(Needle, ESearchCase::IgnoreCase);
		if (Start == INDEX_NONE)
		{
			return FString();
		}

		const FString Tail = Normalized.Mid(Start + Needle.Len());
		TArray<FString> Parts;
		Tail.ParseIntoArray(Parts, TEXT("/"), true);
		return Parts.IsValidIndex(PartIndex) ? Parts[PartIndex] : FString();
	}

	static FString PluginNameFromPath(const FString& Path)
	{
		return ExtractPathPartAfterToolsets(Path, 0);
	}

	static FString ModuleNameFromPath(const FString& Path)
	{
		const FString Normalized = NormalizePathForScan(Path);
		const FString Needle = TEXT("/Source/");
		const int32 Start = Normalized.Find(Needle, ESearchCase::IgnoreCase);
		if (Start == INDEX_NONE)
		{
			return FString();
		}
		const FString Tail = Normalized.Mid(Start + Needle.Len());
		TArray<FString> Parts;
		Tail.ParseIntoArray(Parts, TEXT("/"), true);
		return Parts.Num() > 0 ? Parts[0] : FString();
	}

	static TSharedRef<FJsonObject> PluginProbeJson(const FString& PluginName)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), PluginName);

		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		Obj->SetBoolField(TEXT("found"), Plugin.IsValid());
		if (!Plugin.IsValid())
		{
			Obj->SetBoolField(TEXT("enabled"), false);
			Obj->SetArrayField(TEXT("descriptor_modules"), TArray<TSharedPtr<FJsonValue>>());
			return Obj;
		}

		const FPluginDescriptor& Desc = Plugin->GetDescriptor();
		Obj->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
		Obj->SetStringField(TEXT("friendly_name"), Desc.FriendlyName);
		Obj->SetStringField(TEXT("version_name"), Desc.VersionName);
		Obj->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
		Obj->SetStringField(TEXT("descriptor_file"), Plugin->GetDescriptorFileName());
		Obj->SetBoolField(TEXT("is_beta"), Desc.bIsBetaVersion);
		Obj->SetBoolField(TEXT("is_experimental"), Desc.bIsExperimentalVersion);

		TArray<TSharedPtr<FJsonValue>> ModulesJson;
		TArray<TSharedPtr<FJsonValue>> ExcludedModulesJson;
		for (const FModuleDescriptor& Module : Desc.Modules)
		{
			const FString ModuleName = Module.Name.ToString();
			if (ModuleName.Equals(TEXT("MCPClientToolset"), ESearchCase::IgnoreCase))
			{
				TSharedRef<FJsonObject> ExcludedObj = MakeShared<FJsonObject>();
				ExcludedObj->SetStringField(TEXT("name"), ModuleName);
				ExcludedObj->SetStringField(TEXT("reason"), TEXT("excluded_by_user_request"));
				ExcludedModulesJson.Add(MakeShared<FJsonValueObject>(ExcludedObj));
				continue;
			}
			TSharedRef<FJsonObject> ModuleObj = MakeShared<FJsonObject>();
			ModuleObj->SetStringField(TEXT("name"), ModuleName);
			ModuleObj->SetBoolField(TEXT("exists"), FModuleManager::Get().ModuleExists(*ModuleName));
			ModuleObj->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(FName(*ModuleName)));
			ModulesJson.Add(MakeShared<FJsonValueObject>(ModuleObj));
		}
		Obj->SetArrayField(TEXT("descriptor_modules"), ModulesJson);
		Obj->SetArrayField(TEXT("excluded_descriptor_modules"), ExcludedModulesJson);
		return Obj;
	}

	static TSharedRef<FJsonObject> ModuleProbeJson(const FString& ModuleName)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		FString ModulePath;
		const bool bExists = ModuleExistsCompat(*ModuleName, &ModulePath);
		Obj->SetStringField(TEXT("name"), ModuleName);
		if (ModuleName.Equals(TEXT("MCPClientToolset"), ESearchCase::IgnoreCase))
		{
			Obj->SetBoolField(TEXT("exists"), false);
			Obj->SetBoolField(TEXT("loaded"), false);
			Obj->SetStringField(TEXT("status"), TEXT("excluded_by_user_request"));
			return Obj;
		}
		Obj->SetBoolField(TEXT("exists"), bExists);
		Obj->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(FName(*ModuleName)));
		if (!ModulePath.IsEmpty())
		{
			Obj->SetStringField(TEXT("module_file"), ModulePath);
		}
		return Obj;
	}

	static TSharedRef<FJsonObject> TypeToSchema(const FString& TypeName)
	{
		FString Type = TypeName;
		Type.ReplaceInline(TEXT("const "), TEXT(""));
		Type.ReplaceInline(TEXT("&"), TEXT(""));
		Type.ReplaceInline(TEXT("*"), TEXT(""));
		Type = Type.TrimStartAndEnd();

		if (Type.StartsWith(TEXT("TArray<")))
		{
			return FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TypeName);
		}
		if (Type == TEXT("bool"))
		{
			return FSololmcpSchemaBuilder::Boolean(TypeName);
		}
		if (Type == TEXT("int32") || Type == TEXT("int64") || Type == TEXT("uint32") || Type == TEXT("uint64") || Type == TEXT("uint8") || Type == TEXT("int"))
		{
			return FSololmcpSchemaBuilder::Integer(TypeName);
		}
		if (Type == TEXT("float") || Type == TEXT("double"))
		{
			return FSololmcpSchemaBuilder::Number(TypeName);
		}
		if (Type == TEXT("FString") || Type == TEXT("FName") || Type == TEXT("FText") ||
			Type == TEXT("FSoftObjectPath") || Type == TEXT("FSoftClassPath") ||
			Type.StartsWith(TEXT("TSoftObjectPtr")) || Type.StartsWith(TEXT("TSoftClassPtr")) ||
			Type.StartsWith(TEXT("TObjectPtr")) || Type.StartsWith(TEXT("TSubclassOf")) ||
			Type.StartsWith(TEXT("U")) || Type.StartsWith(TEXT("A")))
		{
			return FSololmcpSchemaBuilder::String(TypeName);
		}
		return FSololmcpSchemaBuilder::Object({}, {}, TypeName);
	}

	static TArray<TSharedPtr<FJsonValue>> ParamsJson(const FToolsetCallable& Callable)
	{
		TArray<TSharedPtr<FJsonValue>> Params;
		for (const TPair<FString, FString>& Param : Callable.Params)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Param.Key);
			Obj->SetStringField(TEXT("type"), Param.Value);
			Obj->SetObjectField(TEXT("schema"), TypeToSchema(Param.Value));
			Params.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Params;
	}

	static TSharedRef<FJsonObject> CallableSchemaJson(const FToolsetCallable& Callable)
	{
		TMap<FString, TSharedRef<FJsonObject>> Properties;
		for (const TPair<FString, FString>& Param : Callable.Params)
		{
			if (!Param.Key.IsEmpty())
			{
				Properties.Add(Param.Key, TypeToSchema(Param.Value));
			}
		}
		return FSololmcpSchemaBuilder::Object(Properties);
	}

	static FString ToSnakeCase(const FString& Name)
	{
		FString Out;
		for (int32 Index = 0; Index < Name.Len(); ++Index)
		{
			const TCHAR Ch = Name[Index];
			if (FChar::IsUpper(Ch))
			{
				if (Index > 0 && Out.Len() > 0 && Out.Right(1) != TEXT("_"))
				{
					Out += TEXT("_");
				}
				Out.AppendChar(FChar::ToLower(Ch));
			}
			else if (Ch == TCHAR('-') || Ch == TCHAR(' ') || Ch == TCHAR('|') || Ch == TCHAR('.'))
			{
				if (Out.Len() > 0 && Out.Right(1) != TEXT("_"))
				{
					Out += TEXT("_");
				}
			}
			else
			{
				Out.AppendChar(FChar::ToLower(Ch));
			}
		}
		return Out;
	}

	static FString InferSafetyClass(const FString& FunctionName, const FString& Category)
	{
		const FString Lower = (FunctionName + TEXT(" ") + Category).ToLower();
		if (Lower.Contains(TEXT("compile")) || Lower.Contains(TEXT("run_test")) || Lower.Contains(TEXT("automation_run")))
		{
			return TEXT("editor_build_or_test");
		}
		if (Lower.StartsWith(TEXT("get")) || Lower.StartsWith(TEXT("list")) || Lower.StartsWith(TEXT("is")) ||
			Lower.StartsWith(TEXT("has")) || Lower.Contains(TEXT("query")) || Lower.Contains(TEXT("inspect")) ||
			Lower.Contains(TEXT("schema")) || Lower.Contains(TEXT("info")))
		{
			return TEXT("read_only");
		}
		if (Lower.StartsWith(TEXT("click")) || Lower.StartsWith(TEXT("type")) || Lower.StartsWith(TEXT("press")) ||
			Lower.StartsWith(TEXT("hover")) || Lower.StartsWith(TEXT("drag")) || Lower.Contains(TEXT("slate")))
		{
			return TEXT("ui_action_guarded");
		}
		return TEXT("mutation_requires_wrapper_receipt");
	}

	static FString WrapperNameForCallable(const FToolsetCallable& Callable)
	{
		return FString::Printf(
			TEXT("ue58_toolset_%s_%s"),
			*ToSnakeCase(Callable.Plugin),
			*ToSnakeCase(Callable.Function));
	}

	static TSharedRef<FJsonObject> WrapperStatusJson(const FToolsetCallable& Callable, const TSet<FString>& RegisteredTools)
	{
		const FString FunctionSnake = ToSnakeCase(Callable.Function);
		const FString Suggested = WrapperNameForCallable(Callable);
		FString MatchedTool;
		if (RegisteredTools.Contains(Suggested))
		{
			MatchedTool = Suggested;
		}
		else if (RegisteredTools.Contains(FunctionSnake))
		{
			MatchedTool = FunctionSnake;
		}
		else
		{
			for (const FString& Registered : RegisteredTools)
			{
				if (Registered.Contains(FunctionSnake) && FunctionSnake.Len() >= 5)
				{
					MatchedTool = Registered;
					break;
				}
			}
		}

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("suggested_wrapper"), Suggested);
		Obj->SetStringField(TEXT("status"), MatchedTool.IsEmpty() ? TEXT("candidate_unwrapped") : TEXT("existing_somolmcp_tool"));
		Obj->SetBoolField(TEXT("covered_by_existing_tool"), !MatchedTool.IsEmpty());
		Obj->SetStringField(TEXT("matched_tool"), MatchedTool);
		Obj->SetStringField(TEXT("implementation_route"), TEXT("SOMOLMCP wrapper only; no MCPClientToolset connection"));
		Obj->SetStringField(TEXT("min_engine_version"), TEXT("5.8.0"));
		return Obj;
	}

	static TSharedRef<FJsonObject> TestPlanJson(const FToolsetCallable& Callable)
	{
		const FString Safety = InferSafetyClass(Callable.Function, Callable.Category);
		TArray<FString> Steps = {
			TEXT("5.7 invocation returns status=requires_ue_5_8 without loading ToolsetRegistry headers."),
			TEXT("5.8 tools/list exposes the SOMOLMCP wrapper or wrapper candidate metadata."),
			TEXT("Schema extraction returns params and JSON schema for the AICallable."),
			TEXT("MCPClientToolset remains excluded and disconnected.")
		};
		if (Safety == TEXT("read_only"))
		{
			Steps.Add(TEXT("Run read-only wrapper smoke with empty or fixture arguments and verify structured response."));
		}
		else
		{
			Steps.Add(TEXT("Dry-run first; mutation requires target binding, scoped transaction, post-readback, validation, and rollback/save receipt."));
		}

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("gate"), Safety == TEXT("read_only") ? TEXT("read_only_smoke") : TEXT("receipt_gated_smoke"));
		Obj->SetArrayField(TEXT("steps"), StringArrayJson(Steps));
		return Obj;
	}

	static TSharedRef<FJsonObject> CallableJson(const FToolsetCallable& Callable, const TSet<FString>& RegisteredTools)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("source"), Callable.Source);
		Obj->SetStringField(TEXT("plugin"), Callable.Plugin);
		Obj->SetStringField(TEXT("module"), Callable.Module);
		Obj->SetStringField(TEXT("class"), Callable.Class);
		Obj->SetStringField(TEXT("function"), Callable.Function);
		Obj->SetStringField(TEXT("category"), Callable.Category);
		Obj->SetStringField(TEXT("return_type"), Callable.ReturnType);
		Obj->SetStringField(TEXT("file"), Callable.FilePath);
		Obj->SetNumberField(TEXT("line"), Callable.Line);
		Obj->SetArrayField(TEXT("params"), ParamsJson(Callable));
		Obj->SetObjectField(TEXT("schema"), CallableSchemaJson(Callable));
		Obj->SetStringField(TEXT("safety"), InferSafetyClass(Callable.Function, Callable.Category));
		Obj->SetObjectField(TEXT("wrapper_status"), WrapperStatusJson(Callable, RegisteredTools));
		Obj->SetObjectField(TEXT("test_plan"), TestPlanJson(Callable));
		return Obj;
	}

	static FString ExtractQuotedValueAfter(const FString& Text, const FString& Key)
	{
		const int32 KeyAt = Text.Find(Key, ESearchCase::IgnoreCase);
		if (KeyAt == INDEX_NONE)
		{
			return FString();
		}
		const int32 FirstQuote = Text.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, KeyAt);
		if (FirstQuote == INDEX_NONE)
		{
			return FString();
		}
		const int32 SecondQuote = Text.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstQuote + 1);
		if (SecondQuote == INDEX_NONE)
		{
			return FString();
		}
		return Text.Mid(FirstQuote + 1, SecondQuote - FirstQuote - 1);
	}

	static FString ExtractFunctionNameFromSignature(const FString& Signature)
	{
		const int32 ParenAt = Signature.Find(TEXT("("));
		if (ParenAt == INDEX_NONE)
		{
			return FString();
		}
		int32 End = ParenAt - 1;
		while (End >= 0 && FChar::IsWhitespace(Signature[End]))
		{
			--End;
		}
		int32 Start = End;
		while (Start >= 0 && (FChar::IsAlnum(Signature[Start]) || Signature[Start] == TCHAR('_')))
		{
			--Start;
		}
		return Signature.Mid(Start + 1, End - Start);
	}

	static FString ExtractReturnTypeFromSignature(const FString& Signature, const FString& FunctionName)
	{
		const int32 NameAt = Signature.Find(FunctionName);
		if (NameAt == INDEX_NONE)
		{
			return FString();
		}
		FString ReturnType = Signature.Left(NameAt);
		ReturnType.ReplaceInline(TEXT("static "), TEXT(""));
		ReturnType.ReplaceInline(TEXT("virtual "), TEXT(""));
		return ReturnType.TrimStartAndEnd();
	}

	static TArray<FString> SplitParams(const FString& ParamText)
	{
		TArray<FString> Params;
		FString Current;
		int32 Depth = 0;
		for (int32 Index = 0; Index < ParamText.Len(); ++Index)
		{
			const TCHAR Ch = ParamText[Index];
			if (Ch == TCHAR('<') || Ch == TCHAR('(') || Ch == TCHAR('['))
			{
				++Depth;
			}
			else if (Ch == TCHAR('>') || Ch == TCHAR(')') || Ch == TCHAR(']'))
			{
				Depth = FMath::Max(0, Depth - 1);
			}
			if (Ch == TCHAR(',') && Depth == 0)
			{
				Params.Add(Current.TrimStartAndEnd());
				Current.Reset();
			}
			else
			{
				Current.AppendChar(Ch);
			}
		}
		if (!Current.TrimStartAndEnd().IsEmpty())
		{
			Params.Add(Current.TrimStartAndEnd());
		}
		return Params;
	}

	static TPair<FString, FString> ParseParam(const FString& RawParam)
	{
		FString Param = RawParam;
		const int32 DefaultAt = Param.Find(TEXT("="));
		if (DefaultAt != INDEX_NONE)
		{
			Param = Param.Left(DefaultAt);
		}
		Param = Param.TrimStartAndEnd();
		if (Param.IsEmpty() || Param == TEXT("void"))
		{
			return TPair<FString, FString>(FString(), FString());
		}

		const int32 UParamAt = Param.Find(TEXT("UPARAM"), ESearchCase::IgnoreCase);
		if (UParamAt != INDEX_NONE)
		{
			const int32 CloseAt = Param.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, UParamAt);
			if (CloseAt != INDEX_NONE)
			{
				Param = (Param.Left(UParamAt) + Param.Mid(CloseAt + 1)).TrimStartAndEnd();
			}
		}

		int32 End = Param.Len() - 1;
		while (End >= 0 && FChar::IsWhitespace(Param[End]))
		{
			--End;
		}
		int32 Start = End;
		while (Start >= 0 && (FChar::IsAlnum(Param[Start]) || Param[Start] == TCHAR('_')))
		{
			--Start;
		}
		const FString Name = Param.Mid(Start + 1, End - Start);
		const FString Type = Param.Left(Start + 1).TrimStartAndEnd();
		return TPair<FString, FString>(Name, Type);
	}

	static void ParseCppFile(const FString& FilePath, TArray<FToolsetCallable>& OutCallables)
	{
		if (IsExcludedToolsetPath(FilePath))
		{
			return;
		}

		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *FilePath))
		{
			return;
		}

		TArray<FString> Lines;
		Contents.ParseIntoArrayLines(Lines, false);

		FString CurrentClass;
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			const FString Line = Lines[Index].TrimStartAndEnd();
			if (Line.StartsWith(TEXT("class ")) && Line.Contains(TEXT("UToolsetDefinition")))
			{
				TArray<FString> Tokens;
				Line.ParseIntoArrayWS(Tokens);
				for (const FString& Token : Tokens)
				{
					if (Token.StartsWith(TEXT("U")) && Token.Contains(TEXT("Tool")))
					{
						CurrentClass = Token;
						break;
					}
				}
			}

			if (!Line.Contains(TEXT("UFUNCTION")) || !Line.Contains(TEXT("AICallable")))
			{
				continue;
			}

			FString Signature;
			int32 SignatureLine = Index + 1;
			for (int32 SigIndex = Index + 1; SigIndex < Lines.Num(); ++SigIndex)
			{
				FString SigLine = Lines[SigIndex].TrimStartAndEnd();
				if (SigLine.IsEmpty() || SigLine.StartsWith(TEXT("//")) || SigLine.StartsWith(TEXT("/*")) || SigLine.StartsWith(TEXT("*")))
				{
					continue;
				}
				if (Signature.IsEmpty())
				{
					SignatureLine = SigIndex + 1;
				}
				Signature += TEXT(" ");
				Signature += SigLine;
				if (SigLine.Contains(TEXT(";")) || SigLine.Contains(TEXT("{")))
				{
					break;
				}
			}

			const FString FunctionName = ExtractFunctionNameFromSignature(Signature);
			if (FunctionName.IsEmpty())
			{
				continue;
			}

			FToolsetCallable Callable;
			Callable.Source = TEXT("source_cpp");
			Callable.Plugin = PluginNameFromPath(FilePath);
			Callable.Module = ModuleNameFromPath(FilePath);
			Callable.Class = CurrentClass;
			Callable.Function = FunctionName;
			Callable.Category = ExtractQuotedValueAfter(Line, TEXT("Category"));
			Callable.ReturnType = ExtractReturnTypeFromSignature(Signature, FunctionName);
			Callable.FilePath = FilePath;
			Callable.Line = SignatureLine;

			const int32 OpenParen = Signature.Find(TEXT("("));
			const int32 CloseParen = Signature.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (OpenParen != INDEX_NONE && CloseParen != INDEX_NONE && CloseParen > OpenParen)
			{
				for (const FString& RawParam : SplitParams(Signature.Mid(OpenParen + 1, CloseParen - OpenParen - 1)))
				{
					const TPair<FString, FString> Parsed = ParseParam(RawParam);
					if (!Parsed.Key.IsEmpty())
					{
						Callable.Params.Add(Parsed);
					}
				}
			}
			OutCallables.Add(Callable);
		}
	}

	static void ParsePythonFile(const FString& FilePath, TArray<FToolsetCallable>& OutCallables)
	{
		if (IsExcludedToolsetPath(FilePath))
		{
			return;
		}

		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *FilePath))
		{
			return;
		}

		TArray<FString> Lines;
		Contents.ParseIntoArrayLines(Lines, false);
		FString CurrentClass;
		bool bNextDefIsToolCall = false;
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			const FString Line = Lines[Index].TrimStartAndEnd();
			if (Line.StartsWith(TEXT("class ")) && Line.Contains(TEXT("ToolsetDefinition")))
			{
				const int32 NameStart = FString(TEXT("class ")).Len();
				const int32 ParenAt = Line.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameStart);
				CurrentClass = ParenAt == INDEX_NONE ? Line.Mid(NameStart) : Line.Mid(NameStart, ParenAt - NameStart);
			}
			if (Line.Contains(TEXT("@toolset_registry.tool_call")))
			{
				bNextDefIsToolCall = true;
				continue;
			}
			if (!bNextDefIsToolCall || !Line.StartsWith(TEXT("def ")))
			{
				continue;
			}
			bNextDefIsToolCall = false;

			const int32 NameStart = FString(TEXT("def ")).Len();
			const int32 ParenAt = Line.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameStart);
			if (ParenAt == INDEX_NONE)
			{
				continue;
			}

			FToolsetCallable Callable;
			Callable.Source = TEXT("source_python");
			Callable.Plugin = PluginNameFromPath(FilePath);
			Callable.Module = ModuleNameFromPath(FilePath);
			Callable.Class = CurrentClass;
			Callable.Function = Line.Mid(NameStart, ParenAt - NameStart);
			Callable.Category = Callable.Plugin;
			Callable.ReturnType = TEXT("python");
			Callable.FilePath = FilePath;
			Callable.Line = Index + 1;

			const int32 CloseParen = Line.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (CloseParen != INDEX_NONE && CloseParen > ParenAt)
			{
				for (const FString& RawParam : SplitParams(Line.Mid(ParenAt + 1, CloseParen - ParenAt - 1)))
				{
					FString Param = RawParam;
					const int32 ColonAt = Param.Find(TEXT(":"));
					const int32 DefaultAt = Param.Find(TEXT("="));
					FString Name = Param;
					FString Type = TEXT("python");
					if (ColonAt != INDEX_NONE)
					{
						Name = Param.Left(ColonAt).TrimStartAndEnd();
						const int32 TypeEnd = DefaultAt != INDEX_NONE ? DefaultAt : Param.Len();
						Type = Param.Mid(ColonAt + 1, TypeEnd - ColonAt - 1).TrimStartAndEnd();
					}
					else if (DefaultAt != INDEX_NONE)
					{
						Name = Param.Left(DefaultAt).TrimStartAndEnd();
					}
					if (!Name.IsEmpty() && Name != TEXT("self"))
					{
						Callable.Params.Add(TPair<FString, FString>(Name, Type));
					}
				}
			}
			OutCallables.Add(Callable);
		}
	}

	static void ScanToolsetSources(TArray<FToolsetCallable>& OutCallables, int32& OutHeaderFiles, int32& OutPythonFiles)
	{
		const FString Root = ToolsetsDir();
		if (!IFileManager::Get().DirectoryExists(*Root))
		{
			return;
		}

		TArray<FString> Headers;
		IFileManager::Get().FindFilesRecursive(Headers, *Root, TEXT("*.h"), true, false);
		Headers.Sort();
		for (const FString& Header : Headers)
		{
			if (IsExcludedToolsetPath(Header))
			{
				continue;
			}
			++OutHeaderFiles;
			ParseCppFile(Header, OutCallables);
		}

		TArray<FString> PythonFiles;
		IFileManager::Get().FindFilesRecursive(PythonFiles, *Root, TEXT("*.py"), true, false);
		PythonFiles.Sort();
		for (const FString& PythonFile : PythonFiles)
		{
			if (IsExcludedToolsetPath(PythonFile))
			{
				continue;
			}
			++OutPythonFiles;
			ParsePythonFile(PythonFile, OutCallables);
		}
	}

	static TMap<FString, FString> BuildModulePluginMap()
	{
		TMap<FString, FString> Map;
		const FString Root = ToolsetsDir();
		if (!IFileManager::Get().DirectoryExists(*Root))
		{
			return Map;
		}

		TArray<FString> PluginFiles;
		IFileManager::Get().FindFilesRecursive(PluginFiles, *Root, TEXT("*.uplugin"), true, false);
		for (const FString& PluginFile : PluginFiles)
		{
			if (IsExcludedToolsetPath(PluginFile))
			{
				continue;
			}
			const FString PluginName = FPaths::GetBaseFilename(PluginFile);
			if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName))
			{
				for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
				{
					const FString ModuleName = Module.Name.ToString();
					if (!ModuleName.Equals(TEXT("MCPClientToolset"), ESearchCase::IgnoreCase))
					{
						Map.Add(ModuleName, PluginName);
					}
				}
			}
		}
		return Map;
	}

	static void ReflectLoadedToolsetCallables(TArray<FToolsetCallable>& OutCallables)
	{
		const TMap<FString, FString> ModulePluginMap = BuildModulePluginMap();
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* Class = *ClassIt;
			if (!Class)
			{
				continue;
			}
			const FString ModuleName = Class->GetOutermost() ? Class->GetOutermost()->GetName().Replace(TEXT("/Script/"), TEXT("")) : FString();
			if (ModuleName.Equals(TEXT("MCPClientToolset"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FString* PluginName = ModulePluginMap.Find(ModuleName);
			if (!PluginName || PluginName->Equals(TEXT("MCPClientToolset"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
			{
				UFunction* Function = *FuncIt;
				if (!Function || !Function->HasMetaData(TEXT("AICallable")))
				{
					continue;
				}

				FToolsetCallable Callable;
				Callable.Source = TEXT("reflection_loaded");
				Callable.Plugin = *PluginName;
				Callable.Module = ModuleName;
				Callable.Class = Class->GetName();
				Callable.Function = Function->GetName();
				Callable.Category = Function->GetMetaData(TEXT("Category"));
				Callable.ReturnType = TEXT("void");
				Callable.FilePath = Class->GetPathName();

				for (TFieldIterator<FProperty> PropIt(Function); PropIt; ++PropIt)
				{
					FProperty* Prop = *PropIt;
					if (!Prop || !Prop->HasAnyPropertyFlags(CPF_Parm))
					{
						continue;
					}
					if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
					{
						Callable.ReturnType = Prop->GetCPPType();
						continue;
					}
					Callable.Params.Add(TPair<FString, FString>(Prop->GetName(), Prop->GetCPPType()));
				}
				OutCallables.Add(Callable);
			}
		}
	}

	static TSet<FString> RegisteredToolSet(FSololmcpToolRegistry* Registry)
	{
		TSet<FString> Names;
		if (!Registry)
		{
			return Names;
		}
		TArray<FString> Registered;
		Registry->GetRegisteredToolNamesSorted(Registered);
		for (const FString& Name : Registered)
		{
			Names.Add(Name.ToLower());
		}
		return Names;
	}

	static bool CallableMatchesFilters(const FToolsetCallable& Callable, const FString& PluginFilter, const FString& ClassFilter, const FString& FunctionFilter)
	{
		return (PluginFilter.IsEmpty() || Callable.Plugin.Contains(PluginFilter, ESearchCase::IgnoreCase)) &&
			(ClassFilter.IsEmpty() || Callable.Class.Contains(ClassFilter, ESearchCase::IgnoreCase)) &&
			(FunctionFilter.IsEmpty() || Callable.Function.Contains(FunctionFilter, ESearchCase::IgnoreCase));
	}

	static void BuildToolsetPluginInventory(TArray<TSharedPtr<FJsonValue>>& OutPlugins, int32& OutExcluded)
	{
		const FString Root = ToolsetsDir();
		if (!IFileManager::Get().DirectoryExists(*Root))
		{
			return;
		}
		TArray<FString> PluginFiles;
		IFileManager::Get().FindFilesRecursive(PluginFiles, *Root, TEXT("*.uplugin"), true, false);
		PluginFiles.Sort();
		for (const FString& PluginFile : PluginFiles)
		{
			const FString PluginName = FPaths::GetBaseFilename(PluginFile);
			if (PluginName.Equals(TEXT("MCPClientToolset"), ESearchCase::IgnoreCase))
			{
				++OutExcluded;
				continue;
			}
			TSharedRef<FJsonObject> PluginObj = PluginProbeJson(PluginName);
			PluginObj->SetStringField(TEXT("descriptor_file"), PluginFile);
			OutPlugins.Add(MakeShared<FJsonValueObject>(PluginObj));
		}
	}

	static TSharedRef<FJsonObject> ToolInputSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("plugin_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional Toolsets plugin substring filter."))},
			{TEXT("class_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional toolset class substring filter."))},
			{TEXT("function_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional callable function substring filter."))},
			{TEXT("include_source"), FSololmcpSchemaBuilder::Boolean(TEXT("Include source scan candidates. Default true."))},
			{TEXT("include_reflection"), FSololmcpSchemaBuilder::Boolean(TEXT("Include loaded UObject reflection candidates. Default true."))},
			{TEXT("max_items"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum callable rows to return. Default 500."))}
		});
	}

	static void SetCommonEnvelope(TSharedRef<FJsonObject>& Out, const FString& ToolName, const FString& Status)
	{
		Out->SetBoolField(TEXT("success"), true);
		Out->SetBoolField(TEXT("read_only"), true);
		Out->SetStringField(TEXT("operation_class"), TEXT("query"));
		Out->SetStringField(TEXT("safety_class"), TEXT("read_only"));
		Out->SetStringField(TEXT("tool_name"), ToolName);
		Out->SetStringField(TEXT("domain"), TEXT("ue58_toolsets"));
		Out->SetStringField(TEXT("status"), Status);
		Out->SetStringField(TEXT("engine_version"), CurrentEngineVersionString());
		Out->SetStringField(TEXT("minimum_engine_version"), TEXT("5.8.0"));
		Out->SetArrayField(TEXT("required_plugins"), StringArrayJson({TEXT("ToolsetRegistry"), TEXT("AllToolsets")}));
		Out->SetArrayField(TEXT("required_modules"), StringArrayJson({TEXT("ToolsetRegistry")}));
		Out->SetBoolField(TEXT("mcp_client_toolset_connected"), false);
		Out->SetStringField(TEXT("mcp_client_toolset_policy"), TEXT("excluded_by_user_request"));
		Out->SetArrayField(TEXT("fallback_tools"), StringArrayJson({TEXT("ue58_toolset_registry_probe"), TEXT("plugin_inspect"), TEXT("tool_schema_repair_candidates")}));
	}

	static bool BuildInventory(
		const FString& ToolName,
		FSololmcpToolRegistry* Registry,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary)
	{
		if (!IsUE58OrLater())
		{
			SetCommonEnvelope(Out, ToolName, TEXT("requires_ue_5_8"));
			Out->SetBoolField(TEXT("available"), false);
			Out->SetBoolField(TEXT("version_satisfied"), false);
			Out->SetArrayField(TEXT("callables"), TArray<TSharedPtr<FJsonValue>>());
			Summary = FString::Printf(TEXT("%s: requires_ue_5_8 on UE %s."), *ToolName, *CurrentEngineVersionString());
			return true;
		}

		FString PluginFilter;
		FString ClassFilter;
		FString FunctionFilter;
		Arguments->TryGetStringField(TEXT("plugin_filter"), PluginFilter);
		Arguments->TryGetStringField(TEXT("class_filter"), ClassFilter);
		Arguments->TryGetStringField(TEXT("function_filter"), FunctionFilter);
		bool bIncludeSource = true;
		bool bIncludeReflection = true;
		Arguments->TryGetBoolField(TEXT("include_source"), bIncludeSource);
		Arguments->TryGetBoolField(TEXT("include_reflection"), bIncludeReflection);
		int32 MaxItems = 500;
		Arguments->TryGetNumberField(TEXT("max_items"), MaxItems);
		MaxItems = FMath::Clamp(MaxItems, 1, 5000);

		TArray<TSharedPtr<FJsonValue>> ToolsetPlugins;
		int32 ExcludedPlugins = 0;
		BuildToolsetPluginInventory(ToolsetPlugins, ExcludedPlugins);

		const bool bToolsetsDirExists = IFileManager::Get().DirectoryExists(*ToolsetsDir());
		const TSharedPtr<IPlugin> ToolsetRegistryPlugin = IPluginManager::Get().FindPlugin(TEXT("ToolsetRegistry"));
		const bool bRegistryPluginFound = ToolsetRegistryPlugin.IsValid();
		const bool bRegistryPluginEnabled = ToolsetRegistryPlugin.IsValid() && ToolsetRegistryPlugin->IsEnabled();
		const bool bRegistryModuleExists = FModuleManager::Get().ModuleExists(TEXT("ToolsetRegistry"));

		FString Status = TEXT("available_probe_only");
		if (!bToolsetsDirExists || !bRegistryPluginFound)
		{
			Status = TEXT("plugin_missing");
		}
		else if (!bRegistryModuleExists)
		{
			Status = TEXT("module_missing");
		}
		else if (!bRegistryPluginEnabled)
		{
			Status = TEXT("plugin_present_not_enabled");
		}

		TArray<FToolsetCallable> Callables;
		int32 HeaderFiles = 0;
		int32 PythonFiles = 0;
		if (bIncludeSource)
		{
			ScanToolsetSources(Callables, HeaderFiles, PythonFiles);
		}
		if (bIncludeReflection)
		{
			ReflectLoadedToolsetCallables(Callables);
		}

		Callables.Sort([](const FToolsetCallable& A, const FToolsetCallable& B)
		{
			const FString AKey = A.Plugin + A.Class + A.Function + A.Source;
			const FString BKey = B.Plugin + B.Class + B.Function + B.Source;
			return AKey < BKey;
		});

		const TSet<FString> RegisteredTools = RegisteredToolSet(Registry);
		TArray<TSharedPtr<FJsonValue>> CallableJsonRows;
		int32 CandidateCount = 0;
		int32 ReflectionCount = 0;
		int32 SourceCount = 0;
		for (const FToolsetCallable& Callable : Callables)
		{
			if (!CallableMatchesFilters(Callable, PluginFilter, ClassFilter, FunctionFilter))
			{
				continue;
			}
			++CandidateCount;
			if (Callable.Source == TEXT("reflection_loaded"))
			{
				++ReflectionCount;
			}
			else
			{
				++SourceCount;
			}
			if (CallableJsonRows.Num() < MaxItems)
			{
				CallableJsonRows.Add(MakeShared<FJsonValueObject>(CallableJson(Callable, RegisteredTools)));
			}
		}

		SetCommonEnvelope(Out, ToolName, Status);
		Out->SetBoolField(TEXT("available"), Status == TEXT("available_probe_only"));
		Out->SetBoolField(TEXT("version_satisfied"), true);
		Out->SetBoolField(TEXT("toolsets_dir_exists"), bToolsetsDirExists);
		Out->SetStringField(TEXT("toolsets_dir"), ToolsetsDir());
		Out->SetObjectField(TEXT("toolset_registry_plugin"), PluginProbeJson(TEXT("ToolsetRegistry")));
		Out->SetObjectField(TEXT("toolset_registry_module"), ModuleProbeJson(TEXT("ToolsetRegistry")));
		Out->SetArrayField(TEXT("toolset_plugins"), ToolsetPlugins);
		Out->SetNumberField(TEXT("excluded_toolset_count"), ExcludedPlugins);
		Out->SetNumberField(TEXT("source_header_file_count"), HeaderFiles);
		Out->SetNumberField(TEXT("source_python_file_count"), PythonFiles);
		Out->SetNumberField(TEXT("candidate_count"), CandidateCount);
		Out->SetNumberField(TEXT("source_candidate_count"), SourceCount);
		Out->SetNumberField(TEXT("reflection_candidate_count"), ReflectionCount);
		Out->SetNumberField(TEXT("returned_count"), CallableJsonRows.Num());
		Out->SetArrayField(TEXT("callables"), CallableJsonRows);

		Summary = FString::Printf(
			TEXT("%s: %s, %d callable candidates returned from %d candidates; MCPClientToolset excluded."),
			*ToolName,
			*Status,
			CallableJsonRows.Num(),
			CandidateCount);
		return true;
	}

	static bool Tool_LiveInventory(
		FSololmcpToolRegistry* Registry,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		return BuildInventory(TEXT("ue58_toolset_registry_live_inventory"), Registry, Arguments, Out, Summary);
	}

	static bool Tool_CallableInventoryCompat(
		FSololmcpToolRegistry* Registry,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		return BuildInventory(TEXT("ue58_toolset_callable_inventory"), Registry, Arguments, Out, Summary);
	}

	static bool Tool_AICallableSchema(
		FSololmcpToolRegistry* Registry,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		if (!BuildInventory(TEXT("ue58_toolset_aicallable_schema"), Registry, Arguments, Out, Summary))
		{
			return false;
		}
		Summary = FString::Printf(TEXT("ue58_toolset_aicallable_schema: %s schemas returned."), *FString::FromInt(static_cast<int32>(Out->GetArrayField(TEXT("callables")).Num())));
		return true;
	}

	static bool Tool_WrapperStatus(
		FSololmcpToolRegistry* Registry,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		if (!BuildInventory(TEXT("ue58_toolset_wrapper_status"), Registry, Arguments, Out, Summary))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Callables = nullptr;
		int32 Covered = 0;
		int32 Unwrapped = 0;
		if (Out->TryGetArrayField(TEXT("callables"), Callables) && Callables)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Callables)
			{
				const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
				const TSharedPtr<FJsonObject>* StatusObj = nullptr;
				bool bCovered = false;
				if (Obj.IsValid() && Obj->TryGetObjectField(TEXT("wrapper_status"), StatusObj) && StatusObj && StatusObj->IsValid() &&
					(*StatusObj)->TryGetBoolField(TEXT("covered_by_existing_tool"), bCovered) && bCovered)
				{
					++Covered;
				}
				else
				{
					++Unwrapped;
				}
			}
		}
		Out->SetNumberField(TEXT("covered_returned_count"), Covered);
		Out->SetNumberField(TEXT("unwrapped_returned_count"), Unwrapped);
		Summary = FString::Printf(TEXT("ue58_toolset_wrapper_status: %d covered, %d candidate-unwrapped rows returned."), Covered, Unwrapped);
		return true;
	}

	static bool Tool_WrapperSmokeMatrix(
		FSololmcpToolRegistry* Registry,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		if (!BuildInventory(TEXT("ue58_toolset_wrapper_smoke_matrix"), Registry, Arguments, Out, Summary))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Callables = nullptr;
		TMap<FString, int32> SafetyCounts;
		TArray<TSharedPtr<FJsonValue>> Matrix;
		if (Out->TryGetArrayField(TEXT("callables"), Callables) && Callables)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Callables)
			{
				const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Obj.IsValid())
				{
					continue;
				}
				FString Safety;
				Obj->TryGetStringField(TEXT("safety"), Safety);
				SafetyCounts.FindOrAdd(Safety)++;

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				FString Plugin;
				FString Function;
				FString ClassName;
				Obj->TryGetStringField(TEXT("plugin"), Plugin);
				Obj->TryGetStringField(TEXT("class"), ClassName);
				Obj->TryGetStringField(TEXT("function"), Function);
				Row->SetStringField(TEXT("plugin"), Plugin);
				Row->SetStringField(TEXT("class"), ClassName);
				Row->SetStringField(TEXT("function"), Function);
				Row->SetStringField(TEXT("safety"), Safety);
				const TSharedPtr<FJsonObject>* Wrapper = nullptr;
				if (Obj->TryGetObjectField(TEXT("wrapper_status"), Wrapper) && Wrapper && Wrapper->IsValid())
				{
					Row->SetObjectField(TEXT("wrapper_status"), *Wrapper);
				}
				const TSharedPtr<FJsonObject>* Plan = nullptr;
				if (Obj->TryGetObjectField(TEXT("test_plan"), Plan) && Plan && Plan->IsValid())
				{
					Row->SetObjectField(TEXT("test_plan"), *Plan);
				}
				Matrix.Add(MakeShared<FJsonValueObject>(Row));
			}
		}

		TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : SafetyCounts)
		{
			Counts->SetNumberField(Pair.Key.IsEmpty() ? TEXT("unknown") : Pair.Key, Pair.Value);
		}
		Out->SetObjectField(TEXT("safety_counts"), Counts);
		Out->SetArrayField(TEXT("smoke_matrix"), Matrix);
		Summary = FString::Printf(TEXT("ue58_toolset_wrapper_smoke_matrix: %d smoke rows returned."), Matrix.Num());
		return true;
	}
}

void RegisterUE58ToolsetTools(FSololmcpToolRegistry& Registry)
{
	using namespace UE58ToolsetTools;

	auto RegisterTool = [&Registry](
		const FString& Name,
		const FString& Description,
		auto Execute)
	{
		FSololmcpToolDefinition Def;
		Def.Name = Name;
		Def.Description = Description;
		Def.InputSchema = ToolInputSchema();
		Def.Execute = Execute;
		Def.CacheTtlSeconds = 20;
		Registry.Register(Def);
	};

	RegisterTool(
		TEXT("ue58_toolset_registry_live_inventory"),
		TEXT("Inventory UE 5.8 ToolsetRegistry / Toolsets AICallable surfaces via reflection, plugin descriptors, and source scan, excluding MCPClientToolset."),
		[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return Tool_LiveInventory(&Registry, Context, Arguments, Out, Summary, Error);
		});

	RegisterTool(
		TEXT("ue58_toolset_aicallable_schema"),
		TEXT("Return JSON-schema style parameter schemas for UE 5.8 Toolsets AICallable candidates, excluding MCPClientToolset."),
		[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return Tool_AICallableSchema(&Registry, Context, Arguments, Out, Summary, Error);
		});

	RegisterTool(
		TEXT("ue58_toolset_wrapper_status"),
		TEXT("Report SOMOLMCP wrapper coverage status for UE 5.8 Toolsets AICallable candidates, keeping MCPClientToolset excluded."),
		[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return Tool_WrapperStatus(&Registry, Context, Arguments, Out, Summary, Error);
		});

	RegisterTool(
		TEXT("ue58_toolset_wrapper_smoke_matrix"),
		TEXT("Build a fail-closed smoke-test matrix for UE 5.8 Toolsets wrapper candidates and existing SOMOLMCP wrappers."),
		[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return Tool_WrapperSmokeMatrix(&Registry, Context, Arguments, Out, Summary, Error);
		});

	RegisterTool(
		TEXT("ue58_toolset_callable_inventory"),
		TEXT("Compatibility alias upgraded to the UE 5.8 ToolsetRegistry live/source inventory; excludes MCPClientToolset and returns structured 5.7 requires_ue_5_8."),
		[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return Tool_CallableInventoryCompat(&Registry, Context, Arguments, Out, Summary, Error);
		});
}
}
