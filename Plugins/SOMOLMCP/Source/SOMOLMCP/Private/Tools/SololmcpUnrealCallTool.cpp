// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpUnrealCallTool.cpp
//
// v3.14 — Single generic "unreal_call" dispatcher.
//
// Lets a CLIENT-SIDE Python interpreter (or, more typically, an LLM agent that
// composes JSON) drive arbitrary `unreal.*` calls one statement at a time
// without having to ship a dedicated MCP tool per UE Python entry-point.
//
// The implementation delegates the actual work to UE's PythonScriptPlugin via
// `GEngine->Exec(World, "py ...")`. We compose a tiny Python source snippet
// per request that:
//
//   1. Idempotently defines a small `_somolmcp_marshal()` helper for converting
//      Python return values back into JSON-friendly primitives.
//   2. Decodes the caller-supplied positional args + kwargs from a base64-encoded
//      JSON blob (avoids ALL string-escaping concerns).
//   3. Executes the requested op (method-call, constructor, get_attr, set_attr).
//   4. Prints the marshaled result on a single tagged line:
//          __SOMOLMCP_RESULT__<json>
//      which the C++ side then scrapes from the captured log buffer.
//
// We DO NOT manage a long-lived UObject handle store in v1; UObjects are
// returned as `{"__type__":"UObject","path":"...","class":"..."}` and the
// caller can re-resolve them via the `_load_object` helper alias on subsequent
// calls. Handle-id wrapping is left as a future optimization (see TODO below).
//
//
// Registry wiring (see NOTES.md alongside this file):
//   - Add a forward decl  `void RegisterUnrealCallTool(FSololmcpToolRegistry&);`
//     to SololmcpToolRegistry.h (with the other v3.14 forward decls near EOF).
//   - Add `RegisterUnrealCallTool(*this);` to the FSololmcpToolRegistry ctor
//     in SololmcpToolRegistry.cpp, after RegisterLiteScanTools(*this).

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpJsonUtils.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Editor.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Misc/Base64.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Modules/ModuleManager.h"

#include "HAL/PlatformTime.h"

#include "Templates/SharedPointer.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPUnrealCall, Log, All);

namespace UE::SOMOLMCP
{

// ─────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────

static const TCHAR* kResultTag        = TEXT("__SOMOLMCP_RESULT__");
static const TCHAR* kErrorTag         = TEXT("__SOMOLMCP_ERROR__");
static constexpr int32 kMaxPathLen    = 200;
// NOTE: FSololmcpToolDefinition has no per-call timeout field (its trailing
// int32 is `CacheTtlSeconds`). We set TTL to 0 because unreal_call is a fully
// generic mutator — caching its results would be unsound. The "timeout"
// concept from the original spec is therefore advisory only.
static constexpr int32 kCacheTtlSec   = 0;

// Bootstrap python — defined ONCE per Python interpreter session via the
// `_somolmcp_marshal not in dir()` guard. Subsequent calls are a no-op.
//
// IMPORTANT: kept on a single FString (no `\n`-split TEXT() concatenation) so
// that what UE forwards to PythonScriptPlugin via `Exec("py ...")` is a single
// physical line — `Exec("py ...")` does not handle multi-line scripts well in
// all UE versions. We rely on `;` separators + `exec(textwrap.dedent(...))`
// inside a single statement to feed the multi-line bootstrap to Python.
static FString MakeBootstrapBlock()
{
	// We embed the multi-line bootstrap as a triple-quoted Python string and
	// pass it to exec(). This way the OUTER Python statement is a single line
	// from UE's point of view, but Python itself sees a normal multi-line block.
	const FString Body = TEXT(
		"import unreal as _u\n"
		"import json as _j\n"
		"import base64 as _b\n"
		"import traceback as _tb\n"
		"def _somolmcp_marshal(v):\n"
		"    if v is None or isinstance(v, (bool, int, float, str)):\n"
		"        return v\n"
		"    if isinstance(v, (list, tuple, set, frozenset)):\n"
		"        return [_somolmcp_marshal(x) for x in v]\n"
		"    if isinstance(v, dict):\n"
		"        return {str(k): _somolmcp_marshal(x) for k, x in v.items()}\n"
		"    try:\n"
		"        if hasattr(_u, 'Vector') and isinstance(v, _u.Vector):\n"
		"            return {'__type__': 'Vector', 'x': v.x, 'y': v.y, 'z': v.z}\n"
		"    except Exception:\n"
		"        pass\n"
		"    try:\n"
		"        if hasattr(_u, 'Vector2D') and isinstance(v, _u.Vector2D):\n"
		"            return {'__type__': 'Vector2D', 'x': v.x, 'y': v.y}\n"
		"    except Exception:\n"
		"        pass\n"
		"    try:\n"
		"        if hasattr(_u, 'Rotator') and isinstance(v, _u.Rotator):\n"
		"            return {'__type__': 'Rotator', 'pitch': v.pitch, 'yaw': v.yaw, 'roll': v.roll}\n"
		"    except Exception:\n"
		"        pass\n"
		"    try:\n"
		"        if hasattr(_u, 'LinearColor') and isinstance(v, _u.LinearColor):\n"
		"            return {'__type__': 'LinearColor', 'r': v.r, 'g': v.g, 'b': v.b, 'a': v.a}\n"
		"    except Exception:\n"
		"        pass\n"
		"    try:\n"
		"        if hasattr(_u, 'Color') and isinstance(v, _u.Color):\n"
		"            return {'__type__': 'Color', 'r': v.r, 'g': v.g, 'b': v.b, 'a': v.a}\n"
		"    except Exception:\n"
		"        pass\n"
		"    try:\n"
		"        if hasattr(_u, 'Transform') and isinstance(v, _u.Transform):\n"
		"            return {'__type__': 'Transform', 'location': _somolmcp_marshal(v.translation), 'rotation': _somolmcp_marshal(v.rotation), 'scale': _somolmcp_marshal(v.scale3d)}\n"
		"    except Exception:\n"
		"        pass\n"
		"    try:\n"
		"        if hasattr(_u, 'Name') and isinstance(v, _u.Name):\n"
		"            return str(v)\n"
		"    except Exception:\n"
		"        pass\n"
		"    try:\n"
		"        if hasattr(_u, 'Object') and isinstance(v, _u.Object):\n"
		"            try:\n"
		"                cls = v.get_class().get_name()\n"
		"            except Exception:\n"
		"                cls = type(v).__name__\n"
		"            try:\n"
		"                pth = v.get_path_name()\n"
		"            except Exception:\n"
		"                pth = ''\n"
		"            return {'__type__': 'UObject', 'path': pth, 'class': cls}\n"
		"    except Exception:\n"
		"        pass\n"
		"    try:\n"
		"        return repr(v)\n"
		"    except Exception:\n"
		"        return '<unrepr>'\n"
		"def _load_object(path):\n"
		"    try:\n"
		"        return _u.EditorAssetLibrary.load_asset(path)\n"
		"    except Exception:\n"
		"        return None\n"
		"def _find_actor(label):\n"
		"    try:\n"
		"        eas = _u.get_editor_subsystem(_u.EditorActorSubsystem)\n"
		"        for a in eas.get_all_level_actors():\n"
		"            if a.get_actor_label() == label or a.get_name() == label:\n"
		"                return a\n"
		"    except Exception:\n"
		"        pass\n"
		"    return None\n");

	// Build:  exec("""<Body>""") wrapped in idempotency guard.
	// The outer single-quote here-doc avoids escaping issues with Body's
	// internal single-quote characters.
	return FString::Printf(
		TEXT("exec(compile(__import__('base64').b64decode('%s').decode('utf-8'), '<somolmcp_bootstrap>', 'exec')) if '_somolmcp_marshal' not in dir() else None"),
		*FBase64::Encode(Body));
}

// ─────────────────────────────────────────────────────────────────────────
// Helper: log capture (mirrors SololmcpEnhancedTools.cpp::FSololmcpLogCapture
// with looser category filter — we want LogPython lines AND any "print()"
// output that the plugin routes to LogPython.)
// ─────────────────────────────────────────────────────────────────────────

class FUnrealCallLogCapture : public FOutputDevice
{
public:
	FString Captured;
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		const FString CatStr = Category.ToString();
		if (CatStr.Contains(TEXT("Python")) || Verbosity <= ELogVerbosity::Warning)
		{
			Captured.Append(V);
			Captured.Append(TEXT("\n"));
		}
	}
};

// ─────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────

static bool IsSafePath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > kMaxPathLen)
	{
		return false;
	}
	for (TCHAR C : Path)
	{
		const bool bAlnum = (C >= TEXT('A') && C <= TEXT('Z'))
		                 || (C >= TEXT('a') && C <= TEXT('z'))
		                 || (C >= TEXT('0') && C <= TEXT('9'));
		if (!bAlnum && C != TEXT('_') && C != TEXT('.'))
		{
			return false;
		}
	}
	return true;
}

// Rewrite "friendly" helper paths to their underlying unreal.* equivalents.
// Returns false if the path is not under unreal.* AND is not a known helper.
static bool RewritePath(FString& InOutPath, FString& OutError)
{
	// Whitelisted helper names defined in the bootstrap block.
	static const TArray<FString> Helpers = { TEXT("_load_object"), TEXT("_find_actor") };

	for (const FString& H : Helpers)
	{
		if (InOutPath == H || InOutPath.StartsWith(H + TEXT(".")))
		{
			// Helpers are bound at top-level in the python globals; nothing to do.
			return true;
		}
	}

	// Future TODO: handle "h_<digits>." paths via a real handle store.
	if (InOutPath.StartsWith(TEXT("h_")))
	{
		OutError = TEXT("Handle-id paths (h_<id>) are not yet supported in v1. Use an asset path with _load_object instead.");
		return false;
	}

	if (InOutPath.StartsWith(TEXT("unreal.")))
	{
		return true;
	}

	// Plain dotted path → assume it is under `unreal.*`.
	InOutPath = TEXT("unreal.") + InOutPath;
	return true;
}

// Serialize a JSON value to a string for embedding in the Python snippet
// (after base64 encoding). Returns "[]" / "{}" defaults on null inputs.
static FString JsonArrayToString(const TArray<TSharedPtr<FJsonValue>>& Values)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer
		= TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Values, Writer);
	return Out;
}

static FString JsonObjectToString(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		return TEXT("{}");
	}
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer
		= TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Out;
}

// Compose the per-call Python snippet (one logical statement, semicolon-joined)
// that decodes args/kwargs from base64 JSON, performs the requested op, and
// prints the marshaled result on a tagged line.
static FString MakePythonSnippet(
	const FString& Path,
	const FString& Op,
	const FString& ArgsJson,
	const FString& KwargsJson)
{
	const FString ArgsB64   = FBase64::Encode(ArgsJson);
	const FString KwargsB64 = FBase64::Encode(KwargsJson);

	// Build the op-specific call body. Each branch must assign `_result`.
	FString OpBody;
	if (Op == TEXT("get_attr"))
	{
		// path = "owner.attribute" — split on last dot.
		int32 LastDot = INDEX_NONE;
		Path.FindLastChar(TEXT('.'), LastDot);
		if (LastDot == INDEX_NONE)
		{
			// Caller-side validation already ensured a dot exists; defensive fallback.
			OpBody = FString::Printf(TEXT("_result = %s"), *Path);
		}
		else
		{
			const FString Owner = Path.Left(LastDot);
			const FString Attr  = Path.Mid(LastDot + 1);
			OpBody = FString::Printf(TEXT("_result = getattr(%s, '%s')"), *Owner, *Attr);
		}
	}
	else if (Op == TEXT("set_attr"))
	{
		int32 LastDot = INDEX_NONE;
		Path.FindLastChar(TEXT('.'), LastDot);
		if (LastDot == INDEX_NONE)
		{
			OpBody = FString::Printf(TEXT("_result = False"));
		}
		else
		{
			const FString Owner = Path.Left(LastDot);
			const FString Attr  = Path.Mid(LastDot + 1);
			// set_attr expects the new value as args[0].
			OpBody = FString::Printf(
				TEXT("setattr(%s, '%s', _args[0]); _result = True"),
				*Owner, *Attr);
		}
	}
	else
	{
		// "method" or "construct" — semantically identical from Python's POV.
		OpBody = FString::Printf(TEXT("_result = %s(*_args, **_kwargs)"), *Path);
	}

	// Single-statement-friendly composition: decode → call → print result.
	// The whole thing is wrapped in a try/except that emits an error tag on
	// failure; that lets the C++ side surface a clean error message.
	const FString Inner = FString::Printf(TEXT(
		"_args = _j.loads(_b.b64decode('%s').decode('utf-8'));"
		"_kwargs = _j.loads(_b.b64decode('%s').decode('utf-8'));"
		"%s;"
		"print('%s' + _j.dumps(_somolmcp_marshal(_result)))"
		),
		*ArgsB64, *KwargsB64, *OpBody, kResultTag);

	// Wrap inner in try/except that prints an error tag on failure.
	const FString TryWrapped = FString::Printf(TEXT(
		"exec(compile(__import__('base64').b64decode('%s').decode('utf-8'), '<somolmcp_call>', 'exec'))"
		),
		*FBase64::Encode(FString::Printf(TEXT(
			"try:\n"
			"    %s\n"
			"except Exception as _e:\n"
			"    print('%s' + _j.dumps({'type': type(_e).__name__, 'message': str(_e), 'traceback': _tb.format_exc()}))\n"
			),
			*Inner, kErrorTag)));

	return TryWrapped;
}

// Parse the captured log to extract the JSON payload that follows kResultTag
// or kErrorTag. Returns true if a result tag was found.
static bool ExtractTaggedPayload(
	const FString& Captured,
	FString& OutResultJson,
	FString& OutErrorJson)
{
	// Scan line-by-line; tags may appear in the middle of a line if UE prefixes
	// the log line with a category, so we use Find() rather than StartsWith.
	TArray<FString> Lines;
	Captured.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
	for (const FString& Line : Lines)
	{
		int32 IdxRes = Line.Find(kResultTag, ESearchCase::CaseSensitive);
		if (IdxRes != INDEX_NONE)
		{
			OutResultJson = Line.Mid(IdxRes + FString(kResultTag).Len());
			return true;
		}
		int32 IdxErr = Line.Find(kErrorTag, ESearchCase::CaseSensitive);
		if (IdxErr != INDEX_NONE)
		{
			OutErrorJson = Line.Mid(IdxErr + FString(kErrorTag).Len());
		}
	}
	return false;
}

// ─────────────────────────────────────────────────────────────────────────
// Tool handler
// ─────────────────────────────────────────────────────────────────────────

static bool ResultObjectReportsFailure(const TSharedPtr<FJsonObject>& Obj, FString& OutFieldName, FString& OutMessage)
{
	if (!Obj.IsValid())
	{
		return false;
	}

	bool bValue = false;
	if (Obj->TryGetBoolField(TEXT("isError"), bValue) && bValue)
	{
		OutFieldName = TEXT("isError");
	}
	else if (Obj->TryGetBoolField(TEXT("ok"), bValue) && !bValue)
	{
		OutFieldName = TEXT("ok");
	}
	else if (Obj->TryGetBoolField(TEXT("success"), bValue) && !bValue)
	{
		OutFieldName = TEXT("success");
	}
	else
	{
		return false;
	}

	if (!Obj->TryGetStringField(TEXT("error"), OutMessage))
	{
		if (!Obj->TryGetStringField(TEXT("message"), OutMessage))
		{
			Obj->TryGetStringField(TEXT("summary"), OutMessage);
		}
	}
	if (OutMessage.IsEmpty())
	{
		OutMessage = FString::Printf(TEXT("unreal_call inner result reported failure via '%s'."), *OutFieldName);
	}
	return true;
}

static bool Tool_UnrealCall(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("unreal_call must be invoked on the game thread.");
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
		return false;
	}

	// 1. Read & validate `path`.
	FString Path;
	if (!Arguments->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		OutError = TEXT("Missing required argument: path");
		SololmcpError::MissingParam(OutStructured, TEXT("path"));
		return false;
	}
	if (!IsSafePath(Path))
	{
		OutError = FString::Printf(
			TEXT("Path contains illegal characters or is too long (max %d). Allowed: [A-Za-z0-9_.]"),
			kMaxPathLen);
		SololmcpError::Set(OutStructured, TEXT("INVALID_PATH"), TEXT("path"), OutError);
		return false;
	}

	// 2. Read `op` (default "method"); validate against whitelist.
	FString Op = TEXT("method");
	Arguments->TryGetStringField(TEXT("op"), Op);
	if (Op != TEXT("method") && Op != TEXT("construct")
	 && Op != TEXT("get_attr") && Op != TEXT("set_attr"))
	{
		OutError = FString::Printf(TEXT("Invalid op '%s'. Expected: method | construct | get_attr | set_attr"), *Op);
		SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("op"), OutError);
		return false;
	}

	// 3. Read `args` (default []) and `kwargs` (default {}) as raw JSON strings.
	const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
	FString ArgsJson = TEXT("[]");
	if (Arguments->TryGetArrayField(TEXT("args"), ArgsArray) && ArgsArray)
	{
		ArgsJson = JsonArrayToString(*ArgsArray);
	}

	TSharedPtr<FJsonObject> KwargsObj;
	FString KwargsJson = TEXT("{}");
	if (TryGetObjectField(Arguments, TEXT("kwargs"), KwargsObj) && KwargsObj.IsValid())
	{
		KwargsJson = JsonObjectToString(KwargsObj);
	}

	// 4. set_attr requires exactly one positional arg (the new value).
	if (Op == TEXT("set_attr"))
	{
		if (!ArgsArray || ArgsArray->Num() != 1)
		{
			OutError = TEXT("set_attr requires exactly one positional arg (the new value) in 'args'.");
			SololmcpError::Set(OutStructured, TEXT("MISSING_PARAM"), TEXT("args"), OutError);
			return false;
		}
	}

	// 5. get_attr / set_attr require an owner.attribute path.
	if ((Op == TEXT("get_attr") || Op == TEXT("set_attr")) && !Path.Contains(TEXT(".")))
	{
		OutError = FString::Printf(TEXT("op '%s' requires a dotted path 'owner.attribute', got '%s'."), *Op, *Path);
		SololmcpError::Set(OutStructured, TEXT("INVALID_PATH"), TEXT("path"), OutError);
		return false;
	}

	// 6. Rewrite path: helpers stay as-is; everything else is prefixed with
	//    "unreal." (if it isn't already). Reject h_<id> handle paths in v1.
	FString RewrittenPath = Path;
	if (!RewritePath(RewrittenPath, OutError))
	{
		SololmcpError::Set(OutStructured, TEXT("NOT_IMPLEMENTED"), TEXT("path"), OutError);
		return false;
	}

	// 7. Resolve editor world + ensure Python plugin is available.
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || !GEngine)
	{
		OutError = TEXT("No editor world / GEngine available for unreal_call.");
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"));
		return false;
	}

	const bool bPluginLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("PythonScriptPlugin"));
	if (!bPluginLoaded)
	{
		FModuleManager::Get().LoadModule(TEXT("PythonScriptPlugin"));
	}
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("PythonScriptPlugin")))
	{
		OutError = TEXT("PythonScriptPlugin is not available. Enable 'Python Editor Script Plugin' in Project Settings > Plugins > Scripting.");
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
		return false;
	}

	// 8. Compose snippets and execute.
	const FString Bootstrap = MakeBootstrapBlock();
	const FString CallSnip  = MakePythonSnippet(RewrittenPath, Op, ArgsJson, KwargsJson);

	const double StartTime = FPlatformTime::Seconds();

	FUnrealCallLogCapture Capture;
	GLog->AddOutputDevice(&Capture);

	// Bootstrap and call are issued as TWO separate `py` execs — keeps each
	// command on a single physical line for `Exec()`, and lets the bootstrap
	// fail-soft (e.g. if `unreal` isn't importable) before we attempt the call.
	const FString BootstrapCmd = TEXT("py ") + Bootstrap;
	const FString CallCmd      = TEXT("py ") + CallSnip;

	const bool bBootOk = GEngine->Exec(World, *BootstrapCmd);
	bool bCallOk = false;
	if (bBootOk)
	{
		bCallOk = GEngine->Exec(World, *CallCmd);
	}
	GLog->RemoveOutputDevice(&Capture);

	const double DurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	OutStructured->SetNumberField(TEXT("duration_ms"), DurationMs);
	OutStructured->SetStringField(TEXT("path"),         Path);
	OutStructured->SetStringField(TEXT("rewritten_path"), RewrittenPath);
	OutStructured->SetStringField(TEXT("op"),           Op);

	if (!bBootOk)
	{
		OutError = TEXT("Bootstrap python failed (could not import `unreal`?). See log.");
		OutStructured->SetStringField(TEXT("log"), Capture.Captured);
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
		return false;
	}

	// 9. Scan captured log for the result/error tag.
	FString ResultJson, ErrorJson;
	const bool bHaveResult = ExtractTaggedPayload(Capture.Captured, ResultJson, ErrorJson);

	if (!bHaveResult)
	{
		// No result tag — either the python raised before our print, or the
		// Exec() itself failed. Surface what we have.
		if (!ErrorJson.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrObj = ParseJsonObject(ErrorJson);
			if (ErrObj.IsValid())
			{
				OutStructured->SetObjectField(TEXT("python_error"), ErrObj);
				FString Msg;
				ErrObj->TryGetStringField(TEXT("message"), Msg);
				FString Type;
				ErrObj->TryGetStringField(TEXT("type"), Type);
				OutError = FString::Printf(TEXT("Python %s: %s"), *Type, *Msg);
			}
			else
			{
				OutError = FString::Printf(TEXT("Python error: %s"), *ErrorJson);
			}
		}
		else
		{
			OutError = bCallOk
				? TEXT("Call returned no result tag (the requested expression may have returned None silently or the print was suppressed). See 'log'.")
				: TEXT("GEngine->Exec(\"py ...\") returned false. See 'log'.");
		}
		OutStructured->SetStringField(TEXT("log"), Capture.Captured);
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
		return false;
	}

	// 10. Parse the marshaled result and stuff it into "value".
	TSharedPtr<FJsonObject> ResultEnvelope;
	{
		// Result might be ANY JSON value (number / string / array / object / null);
		// wrap it in `{"v": <result>}` so we can use the object deserializer.
		const FString Wrapped = FString::Printf(TEXT("{\"v\":%s}"), *ResultJson);
		ResultEnvelope = ParseJsonObject(Wrapped);
	}

	if (!ResultEnvelope.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to parse marshaled python result as JSON: %s"), *ResultJson);
		OutStructured->SetStringField(TEXT("raw_result"), ResultJson);
		OutStructured->SetStringField(TEXT("log"), Capture.Captured);
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"));
		return false;
	}

	const TSharedPtr<FJsonValue> ValueField = ResultEnvelope->TryGetField(TEXT("v"));
	if (ValueField.IsValid())
	{
		OutStructured->SetField(TEXT("value"), ValueField);
	}
	else
	{
		OutStructured->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
	}

	// Optional convenience: if the result envelope contains a UObject ref,
	// surface its path/class at the top level for easy inspection.
	if (ValueField.IsValid() && ValueField->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>& Obj = ValueField->AsObject();
		FString FailureField, FailureMessage;
		if (ResultObjectReportsFailure(Obj, FailureField, FailureMessage))
		{
			OutStructured->SetStringField(TEXT("inner_failure_field"), FailureField);
			OutStructured->SetStringField(TEXT("log"), Capture.Captured);
			OutError = FailureMessage;
			SololmcpError::Set(OutStructured, TEXT("INNER_CALL_FAILED"), FailureField, OutError);
			return false;
		}

		FString TypeTag;
		if (Obj.IsValid() && Obj->TryGetStringField(TEXT("__type__"), TypeTag) && TypeTag == TEXT("UObject"))
		{
			FString UPath, UClass;
			Obj->TryGetStringField(TEXT("path"),  UPath);
			Obj->TryGetStringField(TEXT("class"), UClass);
			OutStructured->SetStringField(TEXT("uobject_path"),  UPath);
			OutStructured->SetStringField(TEXT("uobject_class"), UClass);
		}
	}

	OutSummary = FString::Printf(
		TEXT("unreal_call %s '%s' OK in %.1f ms"),
		*Op, *Path, DurationMs);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────

void RegisterUnrealCallTool(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("unreal_call"),
		TEXT("Generic single-statement dispatcher for the UE Python `unreal` API. "
		     "Pass `path` (dotted, e.g. 'EditorActorSubsystem.get_all_level_actors' — auto-prefixed with 'unreal.'), "
		     "optional `args` (positional, default []), optional `kwargs` (default {}), and optional `op` "
		     "(method | construct | get_attr | set_attr; default 'method'). Returns `{value, duration_ms, ...}` "
		     "where `value` is the marshaled python return; UObjects come back as "
		     "`{__type__:'UObject', path:'...', class:'...'}` and can be re-resolved via the `_load_object` helper. "
		     "Helpers: `_load_object(asset_path)` and `_find_actor(label)`. Requires PythonScriptPlugin."),
		FSololmcpSchemaBuilder::Object(
			{
				{ TEXT("path"),
					FSololmcpSchemaBuilder::String(TEXT("Required. Dotted python path under unreal.* (e.g. 'EditorActorSubsystem.get_all_level_actors') or a registered helper ('_load_object', '_find_actor'). Allowed chars: [A-Za-z0-9_.] only.")) },
				{ TEXT("args"),
					FSololmcpSchemaBuilder::Array(
						FSololmcpSchemaBuilder::Object({}, {}),
						TEXT("Optional positional args; any JSON values. Default [].")) },
				{ TEXT("kwargs"),
					FSololmcpSchemaBuilder::Object(
						{}, {},
						TEXT("Optional keyword args as JSON object. Default {}.")) },
				{ TEXT("op"),
					FSololmcpSchemaBuilder::String(
						TEXT("Operation kind. Default 'method'."),
						{ TEXT("method"), TEXT("construct"), TEXT("get_attr"), TEXT("set_attr") }) },
			},
			{ TEXT("path") }),
		&Tool_UnrealCall,
		nullptr,             // no IsAvailable preflight
		kCacheTtlSec         // CacheTtlSeconds = 0 → never cached (this tool mutates state)
	});
}

} // namespace UE::SOMOLMCP
