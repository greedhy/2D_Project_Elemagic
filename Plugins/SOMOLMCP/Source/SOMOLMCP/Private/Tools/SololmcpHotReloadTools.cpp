// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpHotReloadTools.cpp — SOMOLMCP P3-6
// ═══════════════════════════════════════════════════════════════════════════════
// MCP Tool Hot-Reload — pragmatic interpretation
// ═══════════════════════════════════════════════════════════════════════════════
// Lets users define a new MCP tool by writing a Python function + name, and the
// registry picks it up live (no UE compile/restart). Works because Python
// execution is already wired via the existing python_execute tool.
//
// 5 tools registered:
//   1. mcp_register_python_tool   — register a new tool backed by a Python function
//   2. mcp_unregister_python_tool — mark a registered tool as removed (responds NOT_FOUND)
//   3. mcp_list_dynamic_tools     — list all currently registered dynamic tools
//   4. mcp_reload_python_tool     — replace the python_code for an existing dynamic tool
//   5. mcp_get_python_tool_source — return the source of a dynamic tool (debugging)
//
// Storage: a static TMap<FString, FDynamicTool> keyed by tool_name, guarded by a
// FCriticalSection. The registry doesn't support tool removal, so unregister just
// drops the entry from the map; the registered lambda still exists but returns
// NOT_FOUND on subsequent invocation.
//
// SECURITY CAVEAT: this gives MCP clients arbitrary Python code execution inside
// the editor. Only enable in development / trusted environments.
// ═══════════════════════════════════════════════════════════════════════════════

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpErrorHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/OutputDevice.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/PlatformFileManager.h"
#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace UE::SOMOLMCP
{
	// ─── Dynamic tool storage ──────────────────────────────────────────────────
	struct FDynamicTool
	{
		FString PythonCode;
		FString Description;
		uint64  RegisteredAtMs = 0;
		TSharedPtr<FJsonObject> ArgsSchema;
	};

	static FCriticalSection&  GetDynamicToolsLock()
	{
		static FCriticalSection Lock;
		return Lock;
	}

	static TMap<FString, FDynamicTool>& GetDynamicToolsMap()
	{
		static TMap<FString, FDynamicTool> Map;
		return Map;
	}

	static uint64 NowMs()
	{
		return static_cast<uint64>(FDateTime::UtcNow().ToUnixTimestamp()) * 1000ULL;
	}

	// ─── Python output capture ────────────────────────────────────────────────
	// We subscribe a FOutputDevice to GLog while running the Python script,
	// then scan the captured text for a sentinel line `__MCP_RESULT__{json}`
	// emitted by the wrapper script. If sentinel parsing fails, we fall back
	// to writing/reading a temp result file (toggled via the wrapper script).
	//
	// TODO(P3-6): The FOutputDevice approach can occasionally drop lines if
	// Python flushes after the GLog->RemoveOutputDevice() call. The temp-file
	// fallback below is the more reliable approach and is what the wrapper
	// currently uses for the actual result transport. The log capture is
	// retained for tracebacks/print() output.
	class FSololmcpPyResultCapture : public FOutputDevice
	{
	public:
		FString Captured;
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			const FString CatStr = Category.ToString();
			// Capture Python categories + warnings/errors
			if (CatStr.Contains(TEXT("Python")) || Verbosity <= ELogVerbosity::Warning)
			{
				Captured.Append(V);
				Captured.Append(TEXT("\n"));
			}
		}
	};

	// ─── Python script escaping ───────────────────────────────────────────────
	// Inject `{user_code}` and `{json_args}` into a wrapper script. We use
	// triple-quoted raw strings on the Python side and ensure the values can't
	// terminate them by escaping any literal `'''`.
	static FString EscapeForTripleSingleQuoted(const FString& In)
	{
		// Replace ''' with '\'\'\'' (joined back together inside the raw string)
		return In.Replace(TEXT("'''"), TEXT("''' + chr(39)*3 + r'''"));
	}

	// Build the wrapper Python script that runs the user's `handle(args)` and
	// writes the JSON result to ResultPath, plus prints a sentinel to LogPython.
	static FString BuildWrapperScript(
		const FString& UserCode,
		const FString& ArgsJson,
		const FString& ResultPath)
	{
		// We use raw triple-quoted strings on Python side for both the JSON args
		// and the user code. The temp result file is the primary transport.
		const FString EscapedArgs   = EscapeForTripleSingleQuoted(ArgsJson);
		const FString EscapedCode   = EscapeForTripleSingleQuoted(UserCode);
		const FString EscapedPath   = ResultPath.Replace(TEXT("\\"), TEXT("/"));

		// NOTE: do not use FString::Format with named args here — Python f-strings
		// and {} braces collide with FStringFormatNamedArguments. Use Printf.
		return FString::Printf(TEXT(
			"import json, traceback, sys\n"
			"__mcp_result_path = r'''%s'''\n"
			"__mcp_args_json = r'''%s'''\n"
			"__mcp_result = {}\n"
			"try:\n"
			"    args = json.loads(__mcp_args_json)\n"
			"    __mcp_user_ns = {}\n"
			"    exec(compile(r'''%s''', '<mcp_dynamic_tool>', 'exec'), __mcp_user_ns)\n"
			"    if 'handle' not in __mcp_user_ns or not callable(__mcp_user_ns['handle']):\n"
			"        raise RuntimeError(\"python_code must define `def handle(args: dict) -> dict`\")\n"
			"    __mcp_ret = __mcp_user_ns['handle'](args)\n"
			"    if not isinstance(__mcp_ret, dict):\n"
			"        raise RuntimeError(\"handle() must return a dict, got %%s\" %% type(__mcp_ret).__name__)\n"
			"    __mcp_result = {'ok': True, 'result': __mcp_ret}\n"
			"except Exception as __mcp_e:\n"
			"    __mcp_result = {'ok': False, 'error': str(__mcp_e), 'traceback': traceback.format_exc()}\n"
			"try:\n"
			"    with open(__mcp_result_path, 'w', encoding='utf-8') as __mcp_f:\n"
			"        json.dump(__mcp_result, __mcp_f)\n"
			"except Exception as __mcp_io_e:\n"
			"    print('__MCP_IO_ERROR__' + str(__mcp_io_e))\n"
			"print('__MCP_RESULT__' + json.dumps({'ok': __mcp_result.get('ok', False)}))\n"
		), *EscapedPath, *EscapedArgs, *EscapedCode);
	}

	// ─── Dynamic tool execute lambda ──────────────────────────────────────────
	// Captures only ToolNameCopy. Looks up the python_code at call-time so that
	// reload + unregister see the latest state.
	static bool ExecuteDynamicPythonTool(
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		// 1. Look up python code under the lock, copy, then release.
		FString PythonCode;
		FString Description;
		{
			FScopeLock Guard(&GetDynamicToolsLock());
			const FDynamicTool* Found = GetDynamicToolsMap().Find(ToolName);
			if (!Found)
			{
				SololmcpError::NotFound(OutStructured, ToolName);
				OutStructured->SetStringField(TEXT("note"), TEXT("tool was unregistered"));
				OutError = FString::Printf(TEXT("Dynamic tool '%s' was unregistered."), *ToolName);
				return false;
			}
			PythonCode  = Found->PythonCode;
			Description = Found->Description;
		}

		// 2. Ensure PythonScriptPlugin is loaded.
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("PythonScriptPlugin")))
		{
			FModuleManager::Get().LoadModule(TEXT("PythonScriptPlugin"));
		}
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("PythonScriptPlugin")))
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("PythonScriptPlugin not available. Enable 'Python Editor Script Plugin' in Project Settings > Plugins > Scripting."));
			OutError = TEXT("PythonScriptPlugin unavailable.");
			return false;
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World || !GEngine)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("No editor world / GEngine available for Python exec."));
			OutError = TEXT("No editor world / GEngine.");
			return false;
		}

		// 3. Serialize arguments to JSON (the python wrapper will json.loads it).
		const FString ArgsJson = ToJsonString(Arguments);

		// 4. Allocate temp result file.
		const FString TempDir = FPaths::ProjectIntermediateDir() / TEXT("SOMOLMCP") / TEXT("HotReload");
		IPlatformFile& PFM = FPlatformFileManager::Get().GetPlatformFile();
		PFM.CreateDirectoryTree(*TempDir);
		const FString ResultPath = TempDir / FString::Printf(TEXT("result_%s.json"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));

		// Best-effort: clear stale file before we run.
		if (PFM.FileExists(*ResultPath))
		{
			PFM.DeleteFile(*ResultPath);
		}

		// 5. Build wrapper, exec via the console "py " path (matches python_execute).
		const FString Wrapper = BuildWrapperScript(PythonCode, ArgsJson, ResultPath);

		// The console "py " command consumes a single-line argument. Execute via
		// IPythonScriptPlugin to get multi-line script support.
		// We use the runtime-loaded module via the console exec approach as a
		// fallback: write the wrapper to a temp .py file and run `py <file>`.
		// This avoids hard-linking against PythonScriptPlugin headers from this
		// translation unit (the .Build.cs already exposes the module).
		const FString ScriptPath = TempDir / FString::Printf(TEXT("script_%s.py"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));

		if (!FFileHelper::SaveStringToFile(Wrapper, *ScriptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				FString::Printf(TEXT("Failed to write temporary Python script to %s."), *ScriptPath));
			OutError = TEXT("Failed to write temporary Python script.");
			return false;
		}
		if (!PFM.FileExists(*ScriptPath) || PFM.FileSize(*ScriptPath) <= 0)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				FString::Printf(TEXT("Temporary Python script was not verifiably written to %s."), *ScriptPath));
			PFM.DeleteFile(*ScriptPath);
			OutError = TEXT("Temporary Python script write verification failed.");
			return false;
		}

		// 6. Exec with log capture.
		FSololmcpPyResultCapture Capture;
		GLog->AddOutputDevice(&Capture);

		const FString Exec = FString::Printf(TEXT("py \"%s\""), *ScriptPath.Replace(TEXT("\""), TEXT("\\\"")));
		const bool bExecOk = GEngine->Exec(World, *Exec);

		GLog->RemoveOutputDevice(&Capture);

		// 7. Read result file.
		FString ResultJson;
		const bool bReadOk = FFileHelper::LoadFileToString(ResultJson, *ResultPath);

		// Clean up temp files (best-effort).
		PFM.DeleteFile(*ScriptPath);
		if (bReadOk)
		{
			PFM.DeleteFile(*ResultPath);
		}

		if (!bExecOk && !bReadOk)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("Python exec failed before producing a result file. See 'python_log' for traceback."));
			OutStructured->SetStringField(TEXT("python_log"), Capture.Captured);
			OutError = TEXT("Python execution failed.");
			return false;
		}

		if (!bReadOk)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("Python script ran but did not produce a result file. The user code may have crashed before writing. See 'python_log'."));
			OutStructured->SetStringField(TEXT("python_log"), Capture.Captured);
			OutError = TEXT("No result file produced.");
			return false;
		}

		TSharedPtr<FJsonObject> Parsed = ParseJsonObject(ResultJson);
		if (!Parsed.IsValid())
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("Failed to parse Python result JSON."));
			OutStructured->SetStringField(TEXT("raw_result"), ResultJson);
			OutStructured->SetStringField(TEXT("python_log"), Capture.Captured);
			OutError = TEXT("Result JSON parse failure.");
			return false;
		}

		bool bOk = false;
		Parsed->TryGetBoolField(TEXT("ok"), bOk);
		if (!bOk)
		{
			FString PyError, PyTraceback;
			Parsed->TryGetStringField(TEXT("error"), PyError);
			Parsed->TryGetStringField(TEXT("traceback"), PyTraceback);
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), PyTraceback);
			OutStructured->SetStringField(TEXT("python_error"), PyError);
			OutStructured->SetStringField(TEXT("python_traceback"), PyTraceback);
			OutError = FString::Printf(TEXT("handle() raised: %s"), *PyError);
			return false;
		}

		// Success — copy `result` dict into OutStructured.
		const TSharedPtr<FJsonObject>* ResultObjPtr = nullptr;
		if (Parsed->TryGetObjectField(TEXT("result"), ResultObjPtr) && ResultObjPtr && ResultObjPtr->IsValid())
		{
			for (const auto& Pair : (*ResultObjPtr)->Values)
			{
				OutStructured->SetField(FString(*Pair.Key), Pair.Value);
			}
		}
		OutStructured->SetStringField(TEXT("_dynamic_tool"), ToolName);
		OutSummary = FString::Printf(TEXT("Dynamic tool '%s' executed (Python ok)."), *ToolName);
		return true;
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// Tool 1: mcp_register_python_tool
	// ═══════════════════════════════════════════════════════════════════════════
	static bool Tool_RegisterPythonTool(
		FSololmcpToolRegistry& Registry,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString ToolName, Description, PythonCode;
		if (!Args->TryGetStringField(TEXT("tool_name"), ToolName) || ToolName.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("tool_name"));
			OutError = TEXT("Missing tool_name."); return false;
		}
		if (!ToolName.StartsWith(TEXT("mcp_user_")))
		{
			SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("tool_name"),
				TEXT("tool_name must start with 'mcp_user_' to namespace dynamic tools."));
			OutError = TEXT("tool_name must start with 'mcp_user_'.");
			return false;
		}
		if (!Args->TryGetStringField(TEXT("description"), Description) || Description.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("description"));
			OutError = TEXT("Missing description."); return false;
		}
		if (!Args->TryGetStringField(TEXT("python_code"), PythonCode) || PythonCode.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("python_code"));
			OutError = TEXT("Missing python_code."); return false;
		}
		if (!PythonCode.Contains(TEXT("def handle")))
		{
			SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("python_code"),
				TEXT("python_code must define `def handle(args: dict) -> dict:`"));
			OutError = TEXT("python_code missing handle().");
			return false;
		}

		TSharedPtr<FJsonObject> ArgsSchema;
		const TSharedPtr<FJsonObject>* SchemaPtr = nullptr;
		if (Args->TryGetObjectField(TEXT("args_schema"), SchemaPtr) && SchemaPtr && SchemaPtr->IsValid())
		{
			ArgsSchema = *SchemaPtr;
		}

		// Store entry under the lock.
		int32 Total = 0;
		bool bWasPresent = false;
		{
			FScopeLock Guard(&GetDynamicToolsLock());
			TMap<FString, FDynamicTool>& Map = GetDynamicToolsMap();
			bWasPresent = Map.Contains(ToolName);

			FDynamicTool Entry;
			Entry.PythonCode      = PythonCode;
			Entry.Description     = Description;
			Entry.RegisteredAtMs  = NowMs();
			Entry.ArgsSchema      = ArgsSchema;
			Map.Add(ToolName, Entry);
			Total = Map.Num();
		}

		// Register with the registry. The lambda captures only the name and looks
		// the python code up under the lock at execution time. Re-registering
		// the same name is fine — the registry's TMap::Add overwrites.
		FSololmcpToolDefinition Def;
		Def.Name        = ToolName;
		Def.Description = FString::Printf(TEXT("[dynamic] %s"), *Description);
		Def.InputSchema = ArgsSchema.IsValid()
			? ArgsSchema.ToSharedRef()
			: FSololmcpSchemaBuilder::Object({}, {});
		const FString ToolNameCopy = ToolName;
		Def.Execute = [ToolNameCopy](
			const FSololmcpToolExecutionContext&,
			const TSharedRef<FJsonObject>& InArgs,
			TSharedRef<FJsonObject>& InOutStructured,
			FString& InOutSummary,
			FString& InOutError) -> bool
		{
			return ExecuteDynamicPythonTool(ToolNameCopy, InArgs, InOutStructured, InOutSummary, InOutError);
		};
		Registry.Register(Def);

		OutStructured->SetStringField(TEXT("tool_name"), ToolName);
		OutStructured->SetBoolField(TEXT("registered"), true);
		OutStructured->SetBoolField(TEXT("was_present"), bWasPresent);
		OutStructured->SetNumberField(TEXT("total_dynamic"), Total);
		OutSummary = FString::Printf(TEXT("Registered dynamic tool '%s' (%s, total=%d)."),
			*ToolName, bWasPresent ? TEXT("replaced") : TEXT("new"), Total);
		return true;
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// Tool 2: mcp_unregister_python_tool
	// ═══════════════════════════════════════════════════════════════════════════
	static bool Tool_UnregisterPythonTool(
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString ToolName;
		if (!Args->TryGetStringField(TEXT("tool_name"), ToolName) || ToolName.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("tool_name"));
			OutError = TEXT("Missing tool_name."); return false;
		}

		bool bRemoved = false;
		int32 Total = 0;
		{
			FScopeLock Guard(&GetDynamicToolsLock());
			TMap<FString, FDynamicTool>& Map = GetDynamicToolsMap();
			bRemoved = (Map.Remove(ToolName) > 0);
			Total = Map.Num();
		}

		if (!bRemoved)
		{
			SololmcpError::NotFound(OutStructured, ToolName);
			OutError = FString::Printf(TEXT("Dynamic tool '%s' was not registered."), *ToolName);
			OutStructured->SetStringField(TEXT("tool_name"), ToolName);
			OutStructured->SetBoolField(TEXT("removed"), false);
			return false;
		}

		// NOTE: registry doesn't support tool removal; the previously registered
		// lambda will still resolve but on next invocation will return NOT_FOUND
		// because GetDynamicToolsMap() no longer holds the entry.
		OutStructured->SetStringField(TEXT("tool_name"), ToolName);
		OutStructured->SetBoolField(TEXT("removed"), true);
		OutStructured->SetNumberField(TEXT("total_dynamic"), Total);
		OutStructured->SetStringField(TEXT("note"),
			TEXT("Tool entry removed from dynamic map. The registry still lists the tool name "
				 "(no removal API), but invocation will respond with error_code=NOT_FOUND."));
		OutSummary = FString::Printf(TEXT("Unregistered dynamic tool '%s' (remaining=%d)."), *ToolName, Total);
		return true;
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// Tool 3: mcp_list_dynamic_tools
	// ═══════════════════════════════════════════════════════════════════════════
	static bool Tool_ListDynamicTools(
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary)
	{
		TArray<TSharedPtr<FJsonValue>> ToolsArr;
		int32 Count = 0;
		{
			FScopeLock Guard(&GetDynamicToolsLock());
			const TMap<FString, FDynamicTool>& Map = GetDynamicToolsMap();
			Count = Map.Num();
			ToolsArr.Reserve(Count);
			for (const TPair<FString, FDynamicTool>& Pair : Map)
			{
				TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("name"), Pair.Key);
				Obj->SetStringField(TEXT("description"), Pair.Value.Description);
				Obj->SetNumberField(TEXT("registered_at_ms"), static_cast<double>(Pair.Value.RegisteredAtMs));
				ToolsArr.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}
		// Sort by name for deterministic listings.
		ToolsArr.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject> OA = A.IsValid() ? A->AsObject() : nullptr;
			const TSharedPtr<FJsonObject> OB = B.IsValid() ? B->AsObject() : nullptr;
			if (!OA.IsValid()) return false;
			if (!OB.IsValid()) return true;
			FString NA, NB;
			OA->TryGetStringField(TEXT("name"), NA);
			OB->TryGetStringField(TEXT("name"), NB);
			return NA < NB;
		});

		OutStructured->SetNumberField(TEXT("count"), Count);
		OutStructured->SetArrayField(TEXT("tools"), ToolsArr);
		OutSummary = FString::Printf(TEXT("%d dynamic tool(s) registered."), Count);
		return true;
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// Tool 4: mcp_reload_python_tool
	// ═══════════════════════════════════════════════════════════════════════════
	static bool Tool_ReloadPythonTool(
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString ToolName, NewCode;
		if (!Args->TryGetStringField(TEXT("tool_name"), ToolName) || ToolName.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("tool_name"));
			OutError = TEXT("Missing tool_name."); return false;
		}
		if (!Args->TryGetStringField(TEXT("python_code"), NewCode) || NewCode.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("python_code"));
			OutError = TEXT("Missing python_code."); return false;
		}
		if (!NewCode.Contains(TEXT("def handle")))
		{
			SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("python_code"),
				TEXT("python_code must define `def handle(args: dict) -> dict:`"));
			OutError = TEXT("python_code missing handle().");
			return false;
		}

		FString NewDescription;
		const bool bHasNewDesc = Args->TryGetStringField(TEXT("description"), NewDescription) && !NewDescription.IsEmpty();

		bool bWasPresent = false;
		{
			FScopeLock Guard(&GetDynamicToolsLock());
			TMap<FString, FDynamicTool>& Map = GetDynamicToolsMap();
			FDynamicTool* Found = Map.Find(ToolName);
			if (Found)
			{
				bWasPresent          = true;
				Found->PythonCode    = NewCode;
				if (bHasNewDesc) { Found->Description = NewDescription; }
				Found->RegisteredAtMs = NowMs();
			}
			else
			{
				// Reload-without-prior-register: create the entry but the registry
				// has no lambda for it yet. Caller must use mcp_register_python_tool
				// instead. We surface this as a clear NOT_FOUND for predictable UX.
			}
		}

		if (!bWasPresent)
		{
			SololmcpError::NotFound(OutStructured, ToolName);
			OutStructured->SetStringField(TEXT("tool_name"), ToolName);
			OutStructured->SetBoolField(TEXT("reloaded"), false);
			OutStructured->SetBoolField(TEXT("was_present"), false);
			OutStructured->SetStringField(TEXT("suggestion"),
				TEXT("Tool not registered yet. Call mcp_register_python_tool first."));
			OutError = FString::Printf(TEXT("Dynamic tool '%s' not registered; cannot reload."), *ToolName);
			return false;
		}

		OutStructured->SetStringField(TEXT("tool_name"), ToolName);
		OutStructured->SetBoolField(TEXT("reloaded"), true);
		OutStructured->SetBoolField(TEXT("was_present"), true);
		OutSummary = FString::Printf(TEXT("Reloaded dynamic tool '%s'%s."),
			*ToolName, bHasNewDesc ? TEXT(" (description updated)") : TEXT(""));
		return true;
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// Tool 5: mcp_get_python_tool_source
	// ═══════════════════════════════════════════════════════════════════════════
	static bool Tool_GetPythonToolSource(
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString ToolName;
		if (!Args->TryGetStringField(TEXT("tool_name"), ToolName) || ToolName.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("tool_name"));
			OutError = TEXT("Missing tool_name."); return false;
		}

		FDynamicTool Snapshot;
		bool bFound = false;
		{
			FScopeLock Guard(&GetDynamicToolsLock());
			const FDynamicTool* Found = GetDynamicToolsMap().Find(ToolName);
			if (Found) { Snapshot = *Found; bFound = true; }
		}

		if (!bFound)
		{
			SololmcpError::NotFound(OutStructured, ToolName);
			OutError = FString::Printf(TEXT("Dynamic tool '%s' not found."), *ToolName);
			return false;
		}

		OutStructured->SetStringField(TEXT("tool_name"), ToolName);
		OutStructured->SetStringField(TEXT("python_code"), Snapshot.PythonCode);
		OutStructured->SetStringField(TEXT("description"), Snapshot.Description);
		OutStructured->SetNumberField(TEXT("registered_at_ms"), static_cast<double>(Snapshot.RegisteredAtMs));
		if (Snapshot.ArgsSchema.IsValid())
		{
			OutStructured->SetObjectField(TEXT("args_schema"), Snapshot.ArgsSchema);
		}
		OutSummary = FString::Printf(TEXT("Source for dynamic tool '%s' (%d chars)."),
			*ToolName, Snapshot.PythonCode.Len());
		return true;
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// RegisterHotReloadTools — called from FSololmcpToolRegistry ctor
	// ═══════════════════════════════════════════════════════════════════════════
	void RegisterHotReloadTools(FSololmcpToolRegistry& Registry)
	{
		// We need a stable reference to the Registry inside the register-tool
		// lambda. The Registry instance is owned by the module (long-lived), and
		// the lambda is invoked synchronously while the registry is alive, so
		// capturing by reference is safe.
		FSololmcpToolRegistry* RegistryPtr = &Registry;

		// ── 1. mcp_register_python_tool ──────────────────────────────────────
		Registry.Register({
			TEXT("mcp_register_python_tool"),
			TEXT("[P3-6] Register a new MCP tool backed by a Python `def handle(args: dict) -> dict` function. "
				 "tool_name must be namespaced under 'mcp_user_'. SECURITY: enables arbitrary Python execution; dev-only."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("tool_name"),   FSololmcpSchemaBuilder::String(TEXT("Must start with 'mcp_user_'."))},
					{TEXT("description"), FSololmcpSchemaBuilder::String(TEXT("Human-readable description shown in tool list."))},
					{TEXT("python_code"), FSololmcpSchemaBuilder::String(TEXT("Python source defining `def handle(args: dict) -> dict:`."))},
					{TEXT("args_schema"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional JSON Schema for the tool's input arguments."))}
				},
				{TEXT("tool_name"), TEXT("description"), TEXT("python_code")}
			),
			[RegistryPtr](const FSololmcpToolExecutionContext&,
				const TSharedRef<FJsonObject>& Args,
				TSharedRef<FJsonObject>& OutStructured,
				FString& OutSummary, FString& OutError)
			{
				return Tool_RegisterPythonTool(*RegistryPtr, Args, OutStructured, OutSummary, OutError);
			},
			nullptr,
			0,
			nullptr,
			true
		});

		// ── 2. mcp_unregister_python_tool ────────────────────────────────────
		Registry.Register({
			TEXT("mcp_unregister_python_tool"),
			TEXT("[P3-6] Remove a previously registered dynamic tool. The registry doesn't support tool removal, "
				 "so the registered tool name will still appear but invocation returns error_code=NOT_FOUND."),
			FSololmcpSchemaBuilder::Object(
				{ {TEXT("tool_name"), FSololmcpSchemaBuilder::String()} },
				{TEXT("tool_name")}
			),
			[](const FSololmcpToolExecutionContext&,
				const TSharedRef<FJsonObject>& Args,
				TSharedRef<FJsonObject>& OutStructured,
				FString& OutSummary, FString& OutError)
			{
				return Tool_UnregisterPythonTool(Args, OutStructured, OutSummary, OutError);
			},
			nullptr,
			0
		});

		// ── 3. mcp_list_dynamic_tools ────────────────────────────────────────
		Registry.Register({
			TEXT("mcp_list_dynamic_tools"),
			TEXT("List currently callable native C++ SOMOLMCP tools with optional prefix and text filters."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("prefix"), FSololmcpSchemaBuilder::String(TEXT("Optional case-insensitive tool-name prefix."))},
					{TEXT("filter"), FSololmcpSchemaBuilder::String(TEXT("Optional case-insensitive substring filter."))},
					{TEXT("limit"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum names returned. Defaults to 200; valid range 1-5000."))}
				},
				{}),
			[&Registry](const FSololmcpToolExecutionContext&,
				const TSharedRef<FJsonObject>& Args,
				TSharedRef<FJsonObject>& OutStructured,
				FString& OutSummary, FString& /*OutError*/)
			{
				FString Prefix;
				FString Filter;
				Args->TryGetStringField(TEXT("prefix"), Prefix);
				Args->TryGetStringField(TEXT("filter"), Filter);
				const int32 Limit = Args->HasTypedField<EJson::Number>(TEXT("limit"))
					? FMath::Clamp(static_cast<int32>(Args->GetNumberField(TEXT("limit"))), 1, 5000)
					: 200;

				TArray<FString> RegisteredNames;
				Registry.GetRegisteredToolNamesSorted(RegisteredNames);
				TArray<TSharedPtr<FJsonValue>> Names;
				int32 MatchCount = 0;
				for (const FString& Name : RegisteredNames)
				{
					if ((!Prefix.IsEmpty() && !Name.StartsWith(Prefix, ESearchCase::IgnoreCase)) ||
						(!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase)))
					{
						continue;
					}
					++MatchCount;
					if (Names.Num() < Limit)
					{
						Names.Add(MakeShared<FJsonValueString>(Name));
					}
				}

				OutStructured->SetArrayField(TEXT("names"), Names);
				OutStructured->SetNumberField(TEXT("returned_count"), Names.Num());
				OutStructured->SetNumberField(TEXT("match_count"), MatchCount);
				OutStructured->SetNumberField(TEXT("registered_native_count"), RegisteredNames.Num());
				OutStructured->SetBoolField(TEXT("truncated"), MatchCount > Names.Num());
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutStructured->SetBoolField(TEXT("verified"), true);
				OutSummary = FString::Printf(TEXT("Listed %d of %d matching native C++ tools."), Names.Num(), MatchCount);
				return true;
			},
			nullptr,
			0,
			nullptr,
			false
		});

		// ── 4. mcp_reload_python_tool ────────────────────────────────────────
		Registry.Register({
			TEXT("mcp_reload_python_tool"),
			TEXT("[P3-6] Replace the python_code (and optionally description) for an existing dynamic tool. "
				 "Returns NOT_FOUND if the tool was never registered."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("tool_name"),   FSololmcpSchemaBuilder::String()},
					{TEXT("python_code"), FSololmcpSchemaBuilder::String(TEXT("New Python source defining `def handle(args: dict) -> dict:`."))},
					{TEXT("description"), FSololmcpSchemaBuilder::String(TEXT("Optional new description."))}
				},
				{TEXT("tool_name"), TEXT("python_code")}
			),
			[](const FSololmcpToolExecutionContext&,
				const TSharedRef<FJsonObject>& Args,
				TSharedRef<FJsonObject>& OutStructured,
				FString& OutSummary, FString& OutError)
			{
				return Tool_ReloadPythonTool(Args, OutStructured, OutSummary, OutError);
			},
			nullptr,
			0
		});

		// ── 5. mcp_get_python_tool_source ────────────────────────────────────
		Registry.Register({
			TEXT("mcp_get_python_tool_source"),
			TEXT("[P3-6] Return the python_code, description, and args_schema for a dynamic tool. Useful for debugging."),
			FSololmcpSchemaBuilder::Object(
				{ {TEXT("tool_name"), FSololmcpSchemaBuilder::String()} },
				{TEXT("tool_name")}
			),
			[](const FSololmcpToolExecutionContext&,
				const TSharedRef<FJsonObject>& Args,
				TSharedRef<FJsonObject>& OutStructured,
				FString& OutSummary, FString& OutError)
			{
				return Tool_GetPythonToolSource(Args, OutStructured, OutSummary, OutError);
			},
			nullptr,
			0
		});
	}
}
