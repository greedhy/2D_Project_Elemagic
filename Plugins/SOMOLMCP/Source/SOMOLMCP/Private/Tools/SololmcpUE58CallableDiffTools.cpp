// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpUE58CallableDiffTools.cpp
// ----------------------------------------------------------------------------
// P0 callable inventory / 5.7-vs-5.8 diff tools.
//
// These tools intentionally avoid UE 5.8-only headers and do not connect to
// Epic's MCPClientToolset. They build planning data from source scans, plugin
// descriptors, and loaded-object reflection so wrapper generation can stay
// SOMOLMCP-native and version-gated.
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
#include "UObject/Class.h"
#include "UObject/FieldIterator.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP
{
namespace UE58CallableDiffTools
{
	struct FCallableRow
	{
		FString Source;
		FString EngineBucket;
		FString Plugin;
		FString Module;
		FString Class;
		FString Function;
		FString Category;
		FString ReturnType;
		FString FilePath;
		int32 Line = 0;
		int32 ParamCount = 0;
		bool bBlueprintCallable = true;
		bool bAICallable = false;
		bool bToolset = false;
	};

	struct FPluginRow
	{
		FString Name;
		FString RelativePath;
		FString FilePath;
		TArray<FString> Modules;
		bool bExcluded = false;
	};

	static FString CurrentEngineVersionString()
	{
		return FEngineVersion::Current().ToString();
	}

	static bool IsUE58OrLater()
	{
		const FEngineVersion Current = FEngineVersion::Current();
		return Current.GetMajor() > 5 || (Current.GetMajor() == 5 && Current.GetMinor() >= 8);
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

	static FString NormalizePath(FString Path)
	{
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		FPaths::NormalizeDirectoryName(Path);
		return Path;
	}

	static bool IsExcludedPath(const FString& Path)
	{
		const FString Normalized = NormalizePath(Path);
		return Normalized.Contains(TEXT("/MCPClientToolset/")) ||
			Normalized.EndsWith(TEXT("/MCPClientToolset.uplugin")) ||
			Normalized.Contains(TEXT("/ModelContextProtocol/")) ||
			Normalized.Contains(TEXT("/Intermediate/")) ||
			Normalized.Contains(TEXT("/Binaries/"));
	}

	static FString EngineDirFromMaybeRoot(FString Raw)
	{
		Raw.TrimStartAndEndInline();
		if (Raw.IsEmpty())
		{
			Raw = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
		}
		Raw = NormalizePath(FPaths::ConvertRelativePathToFull(Raw));
		if (IFileManager::Get().DirectoryExists(*FPaths::Combine(Raw, TEXT("Engine"))))
		{
			return NormalizePath(FPaths::Combine(Raw, TEXT("Engine")));
		}
		return Raw;
	}

	static FString SiblingEngineDir(const FString& VersionName)
	{
		const FString CurrentEngine = EngineDirFromMaybeRoot(FString());
		const FString VersionRoot = NormalizePath(FPaths::GetPath(CurrentEngine));
		const FString UEParent = NormalizePath(FPaths::GetPath(VersionRoot));
		const FString Candidate = NormalizePath(FPaths::Combine(UEParent, VersionName, TEXT("Engine")));
		return IFileManager::Get().DirectoryExists(*Candidate) ? Candidate : FString();
	}

	static FString ArgString(const TSharedRef<FJsonObject>& Arguments, const FString& Field, const FString& Default = FString())
	{
		FString Value = Default;
		Arguments->TryGetStringField(Field, Value);
		return Value;
	}

	static int32 ArgInt(const TSharedRef<FJsonObject>& Arguments, const FString& Field, int32 Default)
	{
		int32 Value = Default;
		Arguments->TryGetNumberField(Field, Value);
		return Value;
	}

	static bool ArgBool(const TSharedRef<FJsonObject>& Arguments, const FString& Field, bool Default)
	{
		bool Value = Default;
		Arguments->TryGetBoolField(Field, Value);
		return Value;
	}

	static FString ExtractPathPartAfter(const FString& Path, const FString& Needle, const int32 PartIndex)
	{
		const FString Normalized = NormalizePath(Path);
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
		const FString AfterPlugins = ExtractPathPartAfter(Path, TEXT("/Plugins/"), 0);
		if (AfterPlugins.IsEmpty())
		{
			return FString();
		}

		const FString Normalized = NormalizePath(Path);
		const int32 SourceAt = Normalized.Find(TEXT("/Source/"), ESearchCase::IgnoreCase);
		if (SourceAt != INDEX_NONE)
		{
			const FString BeforeSource = Normalized.Left(SourceAt);
			TArray<FString> Parts;
			BeforeSource.ParseIntoArray(Parts, TEXT("/"), true);
			return Parts.Num() > 0 ? Parts.Last() : AfterPlugins;
		}
		return AfterPlugins;
	}

	static FString ModuleNameFromPath(const FString& Path)
	{
		return ExtractPathPartAfter(Path, TEXT("/Source/"), 0);
	}

	static FString ToolsetPluginFromPath(const FString& Path)
	{
		return ExtractPathPartAfter(Path, TEXT("/Toolsets/"), 0);
	}

	static FString CategoryFromUFunctionLine(const FString& Line)
	{
		const int32 CategoryAt = Line.Find(TEXT("Category"), ESearchCase::IgnoreCase);
		if (CategoryAt == INDEX_NONE)
		{
			return FString();
		}
		const int32 FirstQuote = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, CategoryAt);
		if (FirstQuote == INDEX_NONE)
		{
			return FString();
		}
		const int32 SecondQuote = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstQuote + 1);
		if (SecondQuote == INDEX_NONE)
		{
			return FString();
		}
		return Line.Mid(FirstQuote + 1, SecondQuote - FirstQuote - 1);
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
		ReturnType.ReplaceInline(TEXT("FORCEINLINE "), TEXT(""));
		return ReturnType.TrimStartAndEnd();
	}

	static int32 CountParams(const FString& Signature)
	{
		const int32 OpenAt = Signature.Find(TEXT("("));
		const int32 CloseAt = Signature.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (OpenAt == INDEX_NONE || CloseAt == INDEX_NONE || CloseAt <= OpenAt + 1)
		{
			return 0;
		}
		const FString Params = Signature.Mid(OpenAt + 1, CloseAt - OpenAt - 1).TrimStartAndEnd();
		if (Params.IsEmpty() || Params == TEXT("void"))
		{
			return 0;
		}
		int32 Count = 1;
		int32 Depth = 0;
		for (int32 Index = 0; Index < Params.Len(); ++Index)
		{
			const TCHAR Ch = Params[Index];
			if (Ch == TCHAR('<') || Ch == TCHAR('(') || Ch == TCHAR('['))
			{
				++Depth;
			}
			else if (Ch == TCHAR('>') || Ch == TCHAR(')') || Ch == TCHAR(']'))
			{
				Depth = FMath::Max(0, Depth - 1);
			}
			else if (Ch == TCHAR(',') && Depth == 0)
			{
				++Count;
			}
		}
		return Count;
	}

	static FString InferClassFromLine(const FString& Line)
	{
		if (!Line.Contains(TEXT("class ")))
		{
			return FString();
		}
		TArray<FString> Tokens;
		Line.ParseIntoArrayWS(Tokens);
		for (const FString& Token : Tokens)
		{
			FString Clean = Token;
			Clean.ReplaceInline(TEXT(":"), TEXT(""));
			if (Clean.StartsWith(TEXT("U")) || Clean.StartsWith(TEXT("A")))
			{
				if (!Clean.EndsWith(TEXT("_API")) && Clean.Len() > 1)
				{
					return Clean;
				}
			}
		}
		return FString();
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
			else if (Ch == TCHAR('-') || Ch == TCHAR(' ') || Ch == TCHAR('|') || Ch == TCHAR('.') || Ch == TCHAR(':'))
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

	static FString InferSafety(const FCallableRow& Row)
	{
		const FString Lower = (Row.Function + TEXT(" ") + Row.Category).ToLower();
		if (Lower.StartsWith(TEXT("get")) || Lower.StartsWith(TEXT("list")) || Lower.StartsWith(TEXT("is")) ||
			Lower.StartsWith(TEXT("has")) || Lower.StartsWith(TEXT("can")) || Lower.Contains(TEXT("inspect")) ||
			Lower.Contains(TEXT("schema")) || Lower.Contains(TEXT("query")) || Lower.Contains(TEXT("info")))
		{
			return TEXT("read_only");
		}
		if (Lower.Contains(TEXT("run")) || Lower.Contains(TEXT("compile")) || Lower.Contains(TEXT("test")))
		{
			return TEXT("editor_test_or_build");
		}
		if (Lower.Contains(TEXT("click")) || Lower.Contains(TEXT("slate")) || Lower.Contains(TEXT("press")) || Lower.Contains(TEXT("drag")))
		{
			return TEXT("ui_action_guarded");
		}
		return TEXT("mutation_requires_named_wrapper");
	}

	static FString SuggestedWrapperName(const FCallableRow& Row)
	{
		const FString Prefix = Row.bToolset ? TEXT("ue58_toolset") : TEXT("ue_callable");
		const FString PluginPart = Row.Plugin.IsEmpty() ? Row.Module : Row.Plugin;
		return FString::Printf(TEXT("%s_%s_%s"), *Prefix, *ToSnakeCase(PluginPart), *ToSnakeCase(Row.Function));
	}

	static FString RowKey(const FCallableRow& Row)
	{
		return FString::Printf(TEXT("%s|%s|%s"), *Row.Module, *Row.Class, *Row.Function);
	}

	static bool MatchesFilters(const FCallableRow& Row, const TSharedRef<FJsonObject>& Arguments)
	{
		const FString PluginFilter = ArgString(Arguments, TEXT("plugin_filter")).ToLower();
		const FString ModuleFilter = ArgString(Arguments, TEXT("module_filter")).ToLower();
		const FString ClassFilter = ArgString(Arguments, TEXT("class_filter")).ToLower();
		const FString FunctionFilter = ArgString(Arguments, TEXT("function_filter")).ToLower();
		if (!PluginFilter.IsEmpty() && !Row.Plugin.ToLower().Contains(PluginFilter))
		{
			return false;
		}
		if (!ModuleFilter.IsEmpty() && !Row.Module.ToLower().Contains(ModuleFilter))
		{
			return false;
		}
		if (!ClassFilter.IsEmpty() && !Row.Class.ToLower().Contains(ClassFilter))
		{
			return false;
		}
		if (!FunctionFilter.IsEmpty() && !Row.Function.ToLower().Contains(FunctionFilter))
		{
			return false;
		}
		return true;
	}

	static void AddRowUnique(const FCallableRow& Row, TMap<FString, FCallableRow>& Rows)
	{
		if (Row.Function.IsEmpty())
		{
			return;
		}
		const FString Key = RowKey(Row);
		if (!Rows.Contains(Key))
		{
			Rows.Add(Key, Row);
		}
	}

	static void ParseHeaderFile(const FString& FilePath, const FString& EngineBucket, TMap<FString, FCallableRow>& Rows)
	{
		if (IsExcludedPath(FilePath))
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
			const FString MaybeClass = InferClassFromLine(Line);
			if (!MaybeClass.IsEmpty())
			{
				CurrentClass = MaybeClass;
			}
			if (!Line.Contains(TEXT("UFUNCTION")) || (!Line.Contains(TEXT("BlueprintCallable")) && !Line.Contains(TEXT("AICallable"))))
			{
				continue;
			}

			FString Signature;
			int32 SignatureLine = Index + 1;
			for (int32 SigIndex = Index + 1; SigIndex < Lines.Num(); ++SigIndex)
			{
				const FString SigLine = Lines[SigIndex].TrimStartAndEnd();
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

			FCallableRow Row;
			Row.Source = TEXT("source_scan");
			Row.EngineBucket = EngineBucket;
			Row.FilePath = FilePath;
			Row.Line = SignatureLine;
			Row.Plugin = PluginNameFromPath(FilePath);
			Row.Module = ModuleNameFromPath(FilePath);
			Row.Class = CurrentClass;
			Row.Function = ExtractFunctionNameFromSignature(Signature);
			Row.ReturnType = ExtractReturnTypeFromSignature(Signature, Row.Function);
			Row.ParamCount = CountParams(Signature);
			Row.Category = CategoryFromUFunctionLine(Line);
			Row.bBlueprintCallable = Line.Contains(TEXT("BlueprintCallable"));
			Row.bAICallable = Line.Contains(TEXT("AICallable"));
			Row.bToolset = !ToolsetPluginFromPath(FilePath).IsEmpty();
			if (Row.Plugin.IsEmpty() && Row.bToolset)
			{
				Row.Plugin = ToolsetPluginFromPath(FilePath);
			}
			AddRowUnique(Row, Rows);
		}
	}

	static TArray<FCallableRow> ScanSourceCallables(const FString& EngineDir, const FString& EngineBucket, const TSharedRef<FJsonObject>& Arguments)
	{
		TMap<FString, FCallableRow> RowMap;
		const FString Scope = ArgString(Arguments, TEXT("scope"), TEXT("plugins")).ToLower();
		const int32 MaxFiles = FMath::Clamp(ArgInt(Arguments, TEXT("max_files"), 3500), 100, 50000);

		TArray<FString> Roots;
		if (Scope == TEXT("all") || Scope == TEXT("source") || Scope == TEXT("runtime"))
		{
			Roots.Add(FPaths::Combine(EngineDir, TEXT("Source")));
		}
		if (Scope == TEXT("all") || Scope == TEXT("plugins") || Scope == TEXT("toolsets"))
		{
			Roots.Add(FPaths::Combine(EngineDir, TEXT("Plugins")));
		}

		int32 Visited = 0;
		for (const FString& Root : Roots)
		{
			if (!IFileManager::Get().DirectoryExists(*Root))
			{
				continue;
			}
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.h"), true, false);
			for (const FString& File : Files)
			{
				if (++Visited > MaxFiles)
				{
					break;
				}
				if (Scope == TEXT("toolsets") && !NormalizePath(File).Contains(TEXT("/Toolsets/")))
				{
					continue;
				}
				ParseHeaderFile(File, EngineBucket, RowMap);
			}
			if (Visited > MaxFiles)
			{
				break;
			}
		}

		TArray<FCallableRow> Rows;
		RowMap.GenerateValueArray(Rows);
		Rows.Sort([](const FCallableRow& A, const FCallableRow& B)
		{
			return RowKey(A) < RowKey(B);
		});
		return Rows;
	}

	static FString ModuleNameForClass(const UClass* Class)
	{
		if (!Class || !Class->GetOutermost())
		{
			return FString();
		}
		const FString Package = Class->GetOutermost()->GetName();
		return Package.StartsWith(TEXT("/Script/")) ? Package.RightChop(8) : Package;
	}

	static FString CategoryForFunction(const UFunction* Function)
	{
#if WITH_METADATA
		const FString Category = Function ? Function->GetMetaData(TEXT("Category")) : FString();
		return Category.IsEmpty() ? TEXT("Uncategorized") : Category;
#else
		return TEXT("metadata_unavailable");
#endif
	}

	static void AddReflectionCallables(TArray<FCallableRow>& Rows, const TSharedRef<FJsonObject>& Arguments)
	{
		TMap<FString, FCallableRow> Map;
		for (const FCallableRow& Row : Rows)
		{
			AddRowUnique(Row, Map);
		}
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* Class = *ClassIt;
			if (!Class)
			{
				continue;
			}
			const FString Module = ModuleNameForClass(Class);
			for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
			{
				UFunction* Function = *FuncIt;
				if (!Function || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
				{
					continue;
				}
				FCallableRow Row;
				Row.Source = TEXT("uobject_reflection");
				Row.EngineBucket = TEXT("current");
				Row.Module = Module;
				Row.Class = Class->GetName();
				Row.Function = Function->GetName();
				Row.Category = CategoryForFunction(Function);
				Row.bBlueprintCallable = true;
				Row.bAICallable = Function->HasMetaData(TEXT("AICallable"));
				Row.ParamCount = 0;
				for (TFieldIterator<FProperty> PropIt(Function); PropIt; ++PropIt)
				{
					const FProperty* Property = *PropIt;
					if (Property && Property->HasAnyPropertyFlags(CPF_Parm) && !Property->HasAnyPropertyFlags(CPF_ReturnParm))
					{
						++Row.ParamCount;
					}
				}
				AddRowUnique(Row, Map);
			}
		}
		Map.GenerateValueArray(Rows);
		Rows.Sort([](const FCallableRow& A, const FCallableRow& B)
		{
			return RowKey(A) < RowKey(B);
		});
	}

	static TSharedRef<FJsonObject> RowJson(const FCallableRow& Row, const TSet<FString>& RegisteredTools)
	{
		const FString Suggested = SuggestedWrapperName(Row);
		const FString FunctionSnake = ToSnakeCase(Row.Function);
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
			for (const FString& ToolName : RegisteredTools)
			{
				if (FunctionSnake.Len() >= 5 && ToolName.Contains(FunctionSnake))
				{
					MatchedTool = ToolName;
					break;
				}
			}
		}

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("source"), Row.Source);
		Obj->SetStringField(TEXT("engine_bucket"), Row.EngineBucket);
		Obj->SetStringField(TEXT("plugin"), Row.Plugin);
		Obj->SetStringField(TEXT("module"), Row.Module);
		Obj->SetStringField(TEXT("class"), Row.Class);
		Obj->SetStringField(TEXT("function"), Row.Function);
		Obj->SetStringField(TEXT("category"), Row.Category);
		Obj->SetStringField(TEXT("return_type"), Row.ReturnType);
		Obj->SetStringField(TEXT("file"), Row.FilePath);
		Obj->SetNumberField(TEXT("line"), Row.Line);
		Obj->SetNumberField(TEXT("param_count"), Row.ParamCount);
		Obj->SetBoolField(TEXT("blueprint_callable"), Row.bBlueprintCallable);
		Obj->SetBoolField(TEXT("ai_callable"), Row.bAICallable);
		Obj->SetBoolField(TEXT("toolset"), Row.bToolset);
		Obj->SetStringField(TEXT("safety"), InferSafety(Row));
		Obj->SetStringField(TEXT("suggested_wrapper"), Suggested);
		Obj->SetStringField(TEXT("wrapper_status"), MatchedTool.IsEmpty() ? TEXT("candidate_unwrapped") : TEXT("existing_somolmcp_tool"));
		Obj->SetStringField(TEXT("matched_tool"), MatchedTool);
		return Obj;
	}

	static TSet<FString> RegisteredToolSet(const FSololmcpToolRegistry* Registry)
	{
		TSet<FString> Set;
		if (!Registry)
		{
			return Set;
		}
		TArray<FString> Names;
		Registry->GetRegisteredToolNamesSorted(Names);
		for (const FString& Name : Names)
		{
			Set.Add(Name);
		}
		return Set;
	}

	static TArray<TSharedPtr<FJsonValue>> RowsJson(const TArray<FCallableRow>& Rows, const TSharedRef<FJsonObject>& Arguments, const TSet<FString>& RegisteredTools, int32& OutFiltered)
	{
		const int32 MaxResults = FMath::Clamp(ArgInt(Arguments, TEXT("max_results"), 250), 1, 5000);
		TArray<TSharedPtr<FJsonValue>> Json;
		OutFiltered = 0;
		for (const FCallableRow& Row : Rows)
		{
			if (!MatchesFilters(Row, Arguments))
			{
				continue;
			}
			++OutFiltered;
			if (Json.Num() < MaxResults)
			{
				Json.Add(MakeShared<FJsonValueObject>(RowJson(Row, RegisteredTools)));
			}
		}
		return Json;
	}

	static TSharedRef<FJsonObject> CommonEnvelope(const FString& ToolName)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("tool_name"), ToolName);
		Obj->SetStringField(TEXT("current_engine_version"), CurrentEngineVersionString());
		Obj->SetBoolField(TEXT("ue58_or_later"), IsUE58OrLater());
		Obj->SetBoolField(TEXT("mcp_client_toolset_connected"), false);
		Obj->SetArrayField(TEXT("excluded_plugins"), StringArrayJson({TEXT("MCPClientToolset"), TEXT("ModelContextProtocol")}));
		return Obj;
	}

	static TSharedRef<FJsonObject> ToolInputSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("scope"), FSololmcpSchemaBuilder::String(TEXT("toolsets, plugins, source, runtime, or all."), {TEXT("toolsets"), TEXT("plugins"), TEXT("source"), TEXT("runtime"), TEXT("all")})},
			{TEXT("baseline_engine_dir"), FSololmcpSchemaBuilder::String(TEXT("Optional UE 5.7 engine root or Engine directory."))},
			{TEXT("candidate_engine_dir"), FSololmcpSchemaBuilder::String(TEXT("Optional UE 5.8 engine root or Engine directory."))},
			{TEXT("plugin_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional plugin substring filter."))},
			{TEXT("module_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional module substring filter."))},
			{TEXT("class_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional class substring filter."))},
			{TEXT("function_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional function substring filter."))},
			{TEXT("include_reflection"), FSololmcpSchemaBuilder::Boolean(TEXT("Include loaded UObject reflection for the current editor."))},
			{TEXT("max_files"), FSololmcpSchemaBuilder::Integer(TEXT("Max header files to scan per engine root."))},
			{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Max rows to return."))}
		});
	}

	static FString ResolveBaselineDir(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Dir = ArgString(Arguments, TEXT("baseline_engine_dir"));
		if (!Dir.IsEmpty())
		{
			return EngineDirFromMaybeRoot(Dir);
		}
		const FString Sibling = SiblingEngineDir(TEXT("5.7.4"));
		return Sibling.IsEmpty() ? EngineDirFromMaybeRoot(FString()) : Sibling;
	}

	static FString ResolveCandidateDir(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Dir = ArgString(Arguments, TEXT("candidate_engine_dir"));
		if (!Dir.IsEmpty())
		{
			return EngineDirFromMaybeRoot(Dir);
		}
		const FString Sibling = SiblingEngineDir(TEXT("5.8.0"));
		return Sibling.IsEmpty() ? EngineDirFromMaybeRoot(FString()) : Sibling;
	}

	static bool ToolInventory(
		const FSololmcpToolRegistry* Registry,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		const FString EngineDir = ResolveCandidateDir(Arguments);
		TArray<FCallableRow> Rows = ScanSourceCallables(EngineDir, IsUE58OrLater() ? TEXT("ue58") : TEXT("current"), Arguments);
		if (ArgBool(Arguments, TEXT("include_reflection"), true))
		{
			AddReflectionCallables(Rows, Arguments);
		}
		int32 Filtered = 0;
		Out = CommonEnvelope(TEXT("ue58_callable_inventory_v2"));
		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("status"), IsUE58OrLater() ? TEXT("ok_ue58") : TEXT("ok_baseline"));
		Out->SetStringField(TEXT("engine_dir"), EngineDir);
		Out->SetNumberField(TEXT("total_candidates"), Rows.Num());
		Out->SetArrayField(TEXT("callables"), RowsJson(Rows, Arguments, RegisteredToolSet(Registry), Filtered));
		Out->SetNumberField(TEXT("filtered_candidates"), Filtered);
		Summary = FString::Printf(TEXT("ue58_callable_inventory_v2 returned %d/%d callable rows."), FMath::Min(Filtered, ArgInt(Arguments, TEXT("max_results"), 250)), Filtered);
		return true;
	}

	static bool ToolDiff(
		const FSololmcpToolRegistry* Registry,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		const FString BaselineDir = ResolveBaselineDir(Arguments);
		const FString CandidateDir = ResolveCandidateDir(Arguments);
		const TArray<FCallableRow> BaseRows = ScanSourceCallables(BaselineDir, TEXT("ue57"), Arguments);
		const TArray<FCallableRow> CandidateRows = ScanSourceCallables(CandidateDir, TEXT("ue58"), Arguments);

		TSet<FString> BaseKeys;
		for (const FCallableRow& Row : BaseRows)
		{
			BaseKeys.Add(RowKey(Row));
		}
		TArray<FCallableRow> Added;
		for (const FCallableRow& Row : CandidateRows)
		{
			if (!BaseKeys.Contains(RowKey(Row)))
			{
				Added.Add(Row);
			}
		}

		int32 Filtered = 0;
		Out = CommonEnvelope(TEXT("ue58_callable_diff_57_58"));
		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("baseline_engine_dir"), BaselineDir);
		Out->SetStringField(TEXT("candidate_engine_dir"), CandidateDir);
		Out->SetNumberField(TEXT("baseline_count"), BaseRows.Num());
		Out->SetNumberField(TEXT("candidate_count"), CandidateRows.Num());
		Out->SetNumberField(TEXT("added_count"), Added.Num());
		Out->SetArrayField(TEXT("added_callables"), RowsJson(Added, Arguments, RegisteredToolSet(Registry), Filtered));
		Out->SetNumberField(TEXT("filtered_added_count"), Filtered);
		Summary = FString::Printf(TEXT("ue58 callable diff: %d baseline, %d candidate, %d added."), BaseRows.Num(), CandidateRows.Num(), Added.Num());
		return true;
	}

	static TArray<FString> ExtractModuleNamesFromPluginJson(const FString& Contents)
	{
		TArray<FString> Modules;
		TArray<FString> Lines;
		Contents.ParseIntoArrayLines(Lines, false);
		bool bInModules = false;
		for (const FString& RawLine : Lines)
		{
			const FString Line = RawLine.TrimStartAndEnd();
			if (Line.Contains(TEXT("\"Modules\"")))
			{
				bInModules = true;
				continue;
			}
			if (bInModules && Line.StartsWith(TEXT("]")))
			{
				break;
			}
			if (bInModules && Line.Contains(TEXT("\"Name\"")))
			{
				const int32 Colon = Line.Find(TEXT(":"));
				const int32 FirstQuote = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Colon == INDEX_NONE ? 0 : Colon);
				const int32 SecondQuote = FirstQuote == INDEX_NONE ? INDEX_NONE : Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstQuote + 1);
				if (FirstQuote != INDEX_NONE && SecondQuote != INDEX_NONE)
				{
					Modules.Add(Line.Mid(FirstQuote + 1, SecondQuote - FirstQuote - 1));
				}
			}
		}
		return Modules;
	}

	static TArray<FPluginRow> ScanPluginRows(const FString& EngineDir)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *FPaths::Combine(EngineDir, TEXT("Plugins")), TEXT("*.uplugin"), true, false);
		TArray<FPluginRow> Rows;
		for (const FString& File : Files)
		{
			FPluginRow Row;
			Row.FilePath = File;
			Row.Name = FPaths::GetBaseFilename(File);
			Row.RelativePath = NormalizePath(File).RightChop(NormalizePath(EngineDir).Len());
			Row.bExcluded = IsExcludedPath(File);
			FString Contents;
			if (FFileHelper::LoadFileToString(Contents, *File))
			{
				Row.Modules = ExtractModuleNamesFromPluginJson(Contents);
			}
			Rows.Add(Row);
		}
		Rows.Sort([](const FPluginRow& A, const FPluginRow& B)
		{
			return A.Name < B.Name;
		});
		return Rows;
	}

	static TSharedRef<FJsonObject> PluginRowJson(const FPluginRow& Row)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Row.Name);
		Obj->SetStringField(TEXT("relative_path"), Row.RelativePath);
		Obj->SetStringField(TEXT("file"), Row.FilePath);
		Obj->SetArrayField(TEXT("modules"), StringArrayJson(Row.Modules));
		Obj->SetBoolField(TEXT("excluded_by_policy"), Row.bExcluded);
		return Obj;
	}

	static bool ToolPluginDelta(
		const FSololmcpToolRegistry*,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		const FString BaselineDir = ResolveBaselineDir(Arguments);
		const FString CandidateDir = ResolveCandidateDir(Arguments);
		const TArray<FPluginRow> BasePlugins = ScanPluginRows(BaselineDir);
		const TArray<FPluginRow> CandidatePlugins = ScanPluginRows(CandidateDir);

		TSet<FString> BaseNames;
		for (const FPluginRow& Row : BasePlugins)
		{
			BaseNames.Add(Row.Name);
		}
		TArray<TSharedPtr<FJsonValue>> AddedJson;
		TArray<TSharedPtr<FJsonValue>> ExcludedJson;
		for (const FPluginRow& Row : CandidatePlugins)
		{
			if (Row.bExcluded)
			{
				ExcludedJson.Add(MakeShared<FJsonValueObject>(PluginRowJson(Row)));
				continue;
			}
			if (!BaseNames.Contains(Row.Name))
			{
				AddedJson.Add(MakeShared<FJsonValueObject>(PluginRowJson(Row)));
			}
		}

		Out = CommonEnvelope(TEXT("ue58_plugin_delta_report"));
		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("baseline_engine_dir"), BaselineDir);
		Out->SetStringField(TEXT("candidate_engine_dir"), CandidateDir);
		Out->SetNumberField(TEXT("baseline_plugin_count"), BasePlugins.Num());
		Out->SetNumberField(TEXT("candidate_plugin_count"), CandidatePlugins.Num());
		Out->SetNumberField(TEXT("added_plugin_count"), AddedJson.Num());
		Out->SetArrayField(TEXT("added_plugins"), AddedJson);
		Out->SetArrayField(TEXT("excluded_plugins_seen"), ExcludedJson);
		Summary = FString::Printf(TEXT("ue58 plugin delta: %d added plugins."), AddedJson.Num());
		return true;
	}

	static bool ToolModuleGate(
		const FSololmcpToolRegistry*,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		const FString EngineDir = ResolveCandidateDir(Arguments);
		const TArray<FCallableRow> Rows = ScanSourceCallables(EngineDir, IsUE58OrLater() ? TEXT("ue58") : TEXT("current"), Arguments);
		TMap<FString, int32> CountsByModule;
		TMap<FString, FString> PluginByModule;
		for (const FCallableRow& Row : Rows)
		{
			if (!MatchesFilters(Row, Arguments))
			{
				continue;
			}
			CountsByModule.FindOrAdd(Row.Module)++;
			if (!Row.Plugin.IsEmpty())
			{
				PluginByModule.FindOrAdd(Row.Module) = Row.Plugin;
			}
		}

		TArray<TSharedPtr<FJsonValue>> Gates;
		for (const TPair<FString, int32>& Pair : CountsByModule)
		{
			const FString Module = Pair.Key;
			FString ModulePath;
			TSharedRef<FJsonObject> Gate = MakeShared<FJsonObject>();
			Gate->SetStringField(TEXT("module"), Module);
			Gate->SetStringField(TEXT("plugin"), PluginByModule.FindRef(Module));
			Gate->SetNumberField(TEXT("callable_count"), Pair.Value);
			Gate->SetBoolField(TEXT("module_exists_current"), ModuleExistsCompat(*Module, &ModulePath));
			Gate->SetBoolField(TEXT("module_loaded_current"), FModuleManager::Get().IsModuleLoaded(FName(*Module)));
			if (!ModulePath.IsEmpty())
			{
				Gate->SetStringField(TEXT("module_file_current"), ModulePath);
			}
			Gate->SetStringField(TEXT("gate_policy"), Module.IsEmpty() ? TEXT("source_only") : TEXT("plugin_or_module_required"));
			Gates.Add(MakeShared<FJsonValueObject>(Gate));
		}

		Out = CommonEnvelope(TEXT("ue58_module_gate_report"));
		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("engine_dir"), EngineDir);
		Out->SetArrayField(TEXT("module_gates"), Gates);
		Out->SetNumberField(TEXT("module_count"), Gates.Num());
		Summary = FString::Printf(TEXT("ue58 module gate report returned %d module gates."), Gates.Num());
		return true;
	}

	static int32 RankScore(const FCallableRow& Row, const TSet<FString>& RegisteredTools)
	{
		int32 Score = 0;
		if (Row.bAICallable)
		{
			Score += 40;
		}
		if (Row.bToolset)
		{
			Score += 25;
		}
		const FString Safety = InferSafety(Row);
		if (Safety == TEXT("read_only"))
		{
			Score += 20;
		}
		else if (Safety == TEXT("mutation_requires_named_wrapper"))
		{
			Score += 10;
		}
		if (!RegisteredTools.Contains(SuggestedWrapperName(Row)) && !RegisteredTools.Contains(ToSnakeCase(Row.Function)))
		{
			Score += 15;
		}
		if (Row.Plugin.Contains(TEXT("Automation")) || Row.Plugin.Contains(TEXT("Dataflow")) || Row.Plugin.Contains(TEXT("UMG")) ||
			Row.Plugin.Contains(TEXT("Physics")) || Row.Plugin.Contains(TEXT("GAS")) || Row.Plugin.Contains(TEXT("Gameplay")))
		{
			Score += 10;
		}
		return Score;
	}

	static bool ToolWrapperRank(
		const FSololmcpToolRegistry* Registry,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		const FString EngineDir = ResolveCandidateDir(Arguments);
		TArray<FCallableRow> Rows = ScanSourceCallables(EngineDir, TEXT("ue58"), Arguments);
		const TSet<FString> RegisteredTools = RegisteredToolSet(Registry);
		Rows.Sort([&RegisteredTools](const FCallableRow& A, const FCallableRow& B)
		{
			const int32 ScoreA = RankScore(A, RegisteredTools);
			const int32 ScoreB = RankScore(B, RegisteredTools);
			return ScoreA == ScoreB ? RowKey(A) < RowKey(B) : ScoreA > ScoreB;
		});

		const int32 MaxResults = FMath::Clamp(ArgInt(Arguments, TEXT("max_results"), 100), 1, 1000);
		TArray<TSharedPtr<FJsonValue>> Ranked;
		int32 Seen = 0;
		for (const FCallableRow& Row : Rows)
		{
			if (!MatchesFilters(Row, Arguments))
			{
				continue;
			}
			TSharedRef<FJsonObject> Obj = RowJson(Row, RegisteredTools);
			Obj->SetNumberField(TEXT("rank_score"), RankScore(Row, RegisteredTools));
			Obj->SetArrayField(TEXT("recommended_receipt_gates"), StringArrayJson({
				TEXT("target binding"),
				TEXT("dry-run for mutation"),
				TEXT("resource lock for writes"),
				TEXT("post-readback"),
				TEXT("compile/validate/QA receipt where applicable")
			}));
			if (Ranked.Num() < MaxResults)
			{
				Ranked.Add(MakeShared<FJsonValueObject>(Obj));
			}
			++Seen;
		}

		Out = CommonEnvelope(TEXT("ue58_wrapper_candidate_rank"));
		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("engine_dir"), EngineDir);
		Out->SetNumberField(TEXT("candidate_count"), Seen);
		Out->SetArrayField(TEXT("ranked_candidates"), Ranked);
		Summary = FString::Printf(TEXT("ue58 wrapper candidate rank returned %d/%d rows."), Ranked.Num(), Seen);
		return true;
	}
}

void RegisterUE58CallableDiffTools(FSololmcpToolRegistry& Registry)
{
	using namespace UE58CallableDiffTools;

	auto RegisterTool = [&Registry](
		const FString& Name,
		const FString& Description,
		auto Execute,
		int32 CacheTtlSeconds = 60)
	{
		FSololmcpToolDefinition Def;
		Def.Name = Name;
		Def.Description = Description;
		Def.InputSchema = ToolInputSchema();
		Def.Execute = Execute;
		Def.CacheTtlSeconds = CacheTtlSeconds;
		Registry.Register(Def);
	};

	RegisterTool(
		TEXT("ue58_callable_inventory_v2"),
		TEXT("Inventory BlueprintCallable/AICallable surfaces through UE 5.7-safe source scan and current-editor reflection; excludes MCPClientToolset and official MCP."),
		[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return ToolInventory(&Registry, Context, Arguments, Out, Summary, Error);
		});

	RegisterTool(
		TEXT("ue58_callable_diff_57_58"),
		TEXT("Compare UE 5.7 and UE 5.8 BlueprintCallable/AICallable source surfaces and return 5.8-added wrapper candidates."),
		[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return ToolDiff(&Registry, Context, Arguments, Out, Summary, Error);
		});

	RegisterTool(
		TEXT("ue58_plugin_delta_report"),
		TEXT("Compare UE 5.7 and UE 5.8 engine plugin descriptors, excluding MCPClientToolset / official MCP policy paths."),
		[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return ToolPluginDelta(&Registry, Context, Arguments, Out, Summary, Error);
		});

	RegisterTool(
		TEXT("ue58_module_gate_report"),
		TEXT("Build module/plugin gate facts for callable wrapper candidates so 5.7 and 5.8 agents can fail closed on missing modules."),
		[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return ToolModuleGate(&Registry, Context, Arguments, Out, Summary, Error);
		});

	RegisterTool(
		TEXT("ue58_wrapper_candidate_rank"),
		TEXT("Rank UE 5.8 callable wrapper candidates by AI-callable/toolset/read-only/coverage score and attach receipt-gate guidance."),
		[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return ToolWrapperRank(&Registry, Context, Arguments, Out, Summary, Error);
		},
		30);
}
}
