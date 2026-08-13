// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"

namespace SololmcpError {
    /** Set error_code + optional fields on the Structured object before returning false from a tool handler.
     *  Common codes: MISSING_PARAM, INVALID_PATH, NOT_FOUND, INVALID_TYPE, OPERATION_FAILED, NOT_IMPLEMENTED, UE_API_ERROR. */
    inline void Set(const TSharedRef<FJsonObject>& Structured, const FString& Code,
                    const FString& ExpectedParam = TEXT(""),
                    const FString& Suggestion = TEXT("")) {
        Structured->SetStringField(TEXT("error_code"), Code);
        if (!ExpectedParam.IsEmpty()) Structured->SetStringField(TEXT("expected_param"), ExpectedParam);
        if (!Suggestion.IsEmpty()) Structured->SetStringField(TEXT("suggestion"), Suggestion);
    }
    inline void MissingParam(const TSharedRef<FJsonObject>& S, const FString& Name) {
        Set(S, TEXT("MISSING_PARAM"), Name, FString::Printf(TEXT("Add '%s' to arguments."), *Name));
    }
    inline void InvalidPath(const TSharedRef<FJsonObject>& S, const FString& Path) {
        Set(S, TEXT("INVALID_PATH"), TEXT(""), FString::Printf(TEXT("Asset '%s' not found. Verify it starts with /Game/."), *Path));
    }
    inline void NotFound(const TSharedRef<FJsonObject>& S, const FString& What) {
        Set(S, TEXT("NOT_FOUND"), TEXT(""), FString::Printf(TEXT("'%s' was not found in this context."), *What));
    }
    inline void InvalidType(const TSharedRef<FJsonObject>& S, const FString& Param, const FString& Expected) {
        Set(S, TEXT("INVALID_TYPE"), Param, FString::Printf(TEXT("Param '%s' should be of type: %s."), *Param, *Expected));
    }
}
