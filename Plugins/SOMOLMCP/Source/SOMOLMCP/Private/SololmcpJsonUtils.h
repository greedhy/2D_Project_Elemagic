// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Base64.h"

namespace UE::SOMOLMCP
{
	namespace JsonSerializationSafety
	{
		inline TSharedPtr<FJsonValue> NormalizeValue(
			const TSharedPtr<FJsonValue>& Value,
			TSet<const FJsonObject*>& ActiveObjects,
			const int32 Depth);

		inline TSharedRef<FJsonObject> NormalizeObject(
			const TSharedPtr<FJsonObject>& Object,
			TSet<const FJsonObject*>& ActiveObjects,
			const int32 Depth)
		{
			TSharedRef<FJsonObject> Clean = MakeShared<FJsonObject>();
			if (!Object.IsValid() || Depth > 256)
			{
				return Clean;
			}

			const FJsonObject* Identity = Object.Get();
			if (ActiveObjects.Contains(Identity))
			{
				return Clean;
			}
			ActiveObjects.Add(Identity);
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				Clean->SetField(Pair.Key, NormalizeValue(Pair.Value, ActiveObjects, Depth + 1));
			}
			ActiveObjects.Remove(Identity);
			return Clean;
		}

		// The JSON writer asserts when any object field or array slot contains an
		// invalid shared pointer. Tool results can acquire such sparse nodes from
		// reflected values or protocol/job wrappers after the tool itself returns.
		// Deep-clone at the final serialization boundary and preserve every key and
		// array position by replacing an invalid/cyclic/over-depth node with JSON null.
		inline TSharedPtr<FJsonValue> NormalizeValue(
			const TSharedPtr<FJsonValue>& Value,
			TSet<const FJsonObject*>& ActiveObjects,
			const int32 Depth)
		{
			if (!Value.IsValid() || Depth > 256)
			{
				return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
			}

			switch (Value->Type)
			{
			case EJson::String:
				return TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Value->AsString()));
			case EJson::Number:
				if (Value->PreferStringRepresentation())
				{
					return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumberString>(Value->AsString()));
				}
				{
					const double Number = Value->AsNumber();
					return FMath::IsFinite(Number)
						? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Number))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
				}
			case EJson::Boolean:
				return TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(Value->AsBool()));
			case EJson::Array:
			{
				const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
				if (!Value->TryGetArray(Array) || !Array)
				{
					return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
				}
				TArray<TSharedPtr<FJsonValue>> CleanArray;
				CleanArray.Reserve(Array->Num());
				for (const TSharedPtr<FJsonValue>& Element : *Array)
				{
					CleanArray.Add(NormalizeValue(Element, ActiveObjects, Depth + 1));
				}
				return TSharedPtr<FJsonValue>(MakeShared<FJsonValueArray>(MoveTemp(CleanArray)));
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (!Value->TryGetObject(Object) || !Object || !Object->IsValid())
				{
					return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
				}
				if (ActiveObjects.Contains(Object->Get()) || Depth >= 256)
				{
					return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
				}
				return TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(NormalizeObject(*Object, ActiveObjects, Depth + 1)));
			}
			case EJson::Null:
			case EJson::None:
			default:
				return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
			}
		}

		inline TSharedRef<FJsonObject> NormalizeRoot(const TSharedRef<FJsonObject>& Object)
		{
			TSet<const FJsonObject*> ActiveObjects;
			return NormalizeObject(Object.ToSharedPtr(), ActiveObjects, 0);
		}
	}

	inline TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		TSharedPtr<FJsonObject> Obj;
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
		{
			return nullptr;
		}
		return Obj;
	}

	inline FString ToJsonString(const TSharedRef<FJsonObject>& Obj, const bool bPretty = false)
	{
		const TSharedRef<FJsonObject> SafeObj = JsonSerializationSafety::NormalizeRoot(Obj);
		FString Out;
		if (bPretty)
		{
			TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(SafeObj, Writer);
		}
		else
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(SafeObj, Writer);
		}
		return Out;
	}

	inline TSharedPtr<FJsonValue> MakeTextContentValue(const FString& Text)
	{
		TSharedRef<FJsonObject> TextObj = MakeShared<FJsonObject>();
		TextObj->SetStringField(TEXT("type"), TEXT("text"));
		TextObj->SetStringField(TEXT("text"), Text);
		return MakeShared<FJsonValueObject>(TextObj);
	}

	inline TSharedPtr<FJsonValue> MakeImageContentValue(const TArray<uint8>& PngData, const FString& MimeType = TEXT("image/png"))
	{
		TSharedRef<FJsonObject> ImgObj = MakeShared<FJsonObject>();
		ImgObj->SetStringField(TEXT("type"), TEXT("image"));
		ImgObj->SetStringField(TEXT("data"), FBase64::Encode(PngData.GetData(), PngData.Num()));
		ImgObj->SetStringField(TEXT("mimeType"), MimeType);
		return MakeShared<FJsonValueObject>(ImgObj);
	}

	inline TSharedRef<FJsonObject> MakeToolCallResult(const FString& Summary, const TSharedPtr<FJsonObject>& Structured, const bool bIsError)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Content;
		Content.Add(MakeTextContentValue(Summary));

		// Promote _imageContent from Structured into the MCP content array (backward-compatible)
		if (Structured.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ImageArray = nullptr;
			if (Structured->TryGetArrayField(TEXT("_imageContent"), ImageArray) && ImageArray)
			{
				for (const TSharedPtr<FJsonValue>& ImgVal : *ImageArray)
				{
					if (ImgVal.IsValid())
					{
						Content.Add(ImgVal);
					}
				}
				Structured->RemoveField(TEXT("_imageContent"));
			}
		}

		Result->SetArrayField(TEXT("content"), Content);
		Result->SetBoolField(TEXT("isError"), bIsError);
		if (Structured.IsValid())
		{
			Result->SetObjectField(TEXT("structuredContent"), Structured.ToSharedRef());
		}
		return Result;
	}

	inline bool TryGetStringArray(const TSharedRef<FJsonObject>& Object, const FString& FieldName, TArray<FString>& OutArray)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}

		OutArray.Reset();
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StrValue;
			if (!Value.IsValid() || !Value->TryGetString(StrValue))
			{
				return false;
			}
			OutArray.Add(StrValue);
		}
		return true;
	}

	inline bool TryGetObjectField(const TSharedRef<FJsonObject>& Object, const FString& FieldName, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (!Object->TryGetObjectField(FieldName, Found) || !Found || !Found->IsValid())
		{
			return false;
		}
		OutObject = *Found;
		return true;
	}

	// UE 5.7: Overload for TSharedPtr (for cases where the input is already a shared pointer)
	inline bool TryGetObjectField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, TSharedPtr<FJsonObject>& OutObject)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		return TryGetObjectField(Object.ToSharedRef(), FieldName, OutObject);
	}
}
