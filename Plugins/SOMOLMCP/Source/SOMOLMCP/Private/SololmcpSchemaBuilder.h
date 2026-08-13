#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::SOMOLMCP
{
	class FSololmcpSchemaBuilder
	{
	public:
		static TSharedRef<FJsonObject> String(
			const FString& Description = FString(),
			const TArray<FString>& EnumValues = {},
			const int32 MinLength = INDEX_NONE,
			const int32 MaxLength = INDEX_NONE,
			const FString& Pattern = FString(),
			const FString& Format = FString())
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("string"));
			SetDescription(Schema, Description);
			if (!EnumValues.IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> EnumJson;
				for (const FString& Value : EnumValues)
				{
					EnumJson.Add(MakeShared<FJsonValueString>(Value));
				}
				Schema->SetArrayField(TEXT("enum"), EnumJson);
			}
			if (MinLength != INDEX_NONE)
			{
				Schema->SetNumberField(TEXT("minLength"), MinLength);
			}
			if (MaxLength != INDEX_NONE)
			{
				Schema->SetNumberField(TEXT("maxLength"), MaxLength);
			}
			if (!Pattern.IsEmpty())
			{
				Schema->SetStringField(TEXT("pattern"), Pattern);
			}
			if (!Format.IsEmpty())
			{
				Schema->SetStringField(TEXT("format"), Format);
			}
			return Schema;
		}

		static TSharedRef<FJsonObject> Boolean(const FString& Description = FString())
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("boolean"));
			SetDescription(Schema, Description);
			return Schema;
		}

		static TSharedRef<FJsonObject> Null(const FString& Description = FString())
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("null"));
			SetDescription(Schema, Description);
			return Schema;
		}

		static TSharedRef<FJsonObject> Number(
			const FString& Description = FString(),
			const TOptional<double>& Minimum = TOptional<double>(),
			const TOptional<double>& Maximum = TOptional<double>(),
			const bool bExclusiveMinimum = false,
			const bool bExclusiveMaximum = false)
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("number"));
			SetDescription(Schema, Description);
			SetNumericBounds(Schema, Minimum, Maximum, bExclusiveMinimum, bExclusiveMaximum);
			return Schema;
		}

		static TSharedRef<FJsonObject> Integer(
			const FString& Description = FString(),
			const TOptional<int64>& Minimum = TOptional<int64>(),
			const TOptional<int64>& Maximum = TOptional<int64>(),
			const bool bExclusiveMinimum = false,
			const bool bExclusiveMaximum = false)
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("integer"));
			SetDescription(Schema, Description);
			if (Minimum.IsSet())
			{
				Schema->SetNumberField(
					bExclusiveMinimum ? TEXT("exclusiveMinimum") : TEXT("minimum"),
					static_cast<double>(Minimum.GetValue()));
			}
			if (Maximum.IsSet())
			{
				Schema->SetNumberField(
					bExclusiveMaximum ? TEXT("exclusiveMaximum") : TEXT("maximum"),
					static_cast<double>(Maximum.GetValue()));
			}
			return Schema;
		}

		static TSharedRef<FJsonObject> Array(
			const TSharedRef<FJsonObject>& Items,
			const FString& Description = FString(),
			const int32 MinItems = INDEX_NONE,
			const int32 MaxItems = INDEX_NONE,
			const bool bUniqueItems = false)
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("array"));
			Schema->SetObjectField(TEXT("items"), Items);
			SetDescription(Schema, Description);
			if (MinItems != INDEX_NONE)
			{
				Schema->SetNumberField(TEXT("minItems"), MinItems);
			}
			if (MaxItems != INDEX_NONE)
			{
				Schema->SetNumberField(TEXT("maxItems"), MaxItems);
			}
			if (bUniqueItems)
			{
				Schema->SetBoolField(TEXT("uniqueItems"), true);
			}
			return Schema;
		}

		static TSharedRef<FJsonObject> Object(
			const TMap<FString, TSharedRef<FJsonObject>>& Properties,
			const TArray<FString>& Required = {},
			const FString& Description = FString(),
			const bool bAdditionalProperties = true)
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			SetDescription(Schema, Description);

			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedRef<FJsonObject>>& Pair : Properties)
			{
				Props->SetObjectField(Pair.Key, Pair.Value);
			}
			Schema->SetObjectField(TEXT("properties"), Props);

			if (!Required.IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> RequiredJson;
				for (const FString& Field : Required)
				{
					RequiredJson.Add(MakeShared<FJsonValueString>(Field));
				}
				Schema->SetArrayField(TEXT("required"), RequiredJson);
			}
			Schema->SetBoolField(TEXT("additionalProperties"), bAdditionalProperties);
			return Schema;
		}

		static TSharedRef<FJsonObject> Object(
			const TMap<FString, TSharedRef<FJsonObject>>& Properties,
			const TArray<FString>& Required,
			const FString& Description,
			const TOptional<bool>& AdditionalProperties)
		{
			TSharedRef<FJsonObject> Schema = Object(Properties, Required, Description, true);
			if (AdditionalProperties.IsSet())
			{
				Schema->SetBoolField(TEXT("additionalProperties"), AdditionalProperties.GetValue());
			}
			else
			{
				Schema->RemoveField(TEXT("additionalProperties"));
			}
			return Schema;
		}

		static TSharedRef<FJsonObject> Ref(const FString& Reference)
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("$ref"), Reference);
			return Schema;
		}

		static TSharedRef<FJsonObject> AnyOf(const TArray<TSharedRef<FJsonObject>>& Schemas)
		{
			return Composite(TEXT("anyOf"), Schemas);
		}

		static TSharedRef<FJsonObject> OneOf(const TArray<TSharedRef<FJsonObject>>& Schemas)
		{
			return Composite(TEXT("oneOf"), Schemas);
		}

		static TSharedRef<FJsonObject> AllOf(const TArray<TSharedRef<FJsonObject>>& Schemas)
		{
			return Composite(TEXT("allOf"), Schemas);
		}

		static TSharedRef<FJsonObject> Not(const TSharedRef<FJsonObject>& Schema)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("not"), Schema);
			return Result;
		}

		static TSharedRef<FJsonObject> Conditional(
			const TSharedRef<FJsonObject>& IfSchema,
			const TSharedRef<FJsonObject>& ThenSchema,
			const TSharedPtr<FJsonObject>& ElseSchema = nullptr)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("if"), IfSchema);
			Result->SetObjectField(TEXT("then"), ThenSchema);
			if (ElseSchema.IsValid())
			{
				Result->SetObjectField(TEXT("else"), ElseSchema.ToSharedRef());
			}
			return Result;
		}

		static TSharedRef<FJsonObject> WithConstString(
			const TSharedRef<FJsonObject>& Schema,
			const FString& Value)
		{
			Schema->SetStringField(TEXT("const"), Value);
			return Schema;
		}

		static TSharedRef<FJsonObject> WithConstBoolean(
			const TSharedRef<FJsonObject>& Schema,
			const bool Value)
		{
			Schema->SetBoolField(TEXT("const"), Value);
			return Schema;
		}

		static TSharedRef<FJsonObject> WithDefaultBoolean(
			const TSharedRef<FJsonObject>& Schema,
			const bool Value)
		{
			Schema->SetBoolField(TEXT("default"), Value);
			return Schema;
		}

		static TSharedRef<FJsonObject> WithDefaultString(
			const TSharedRef<FJsonObject>& Schema,
			const FString& Value)
		{
			Schema->SetStringField(TEXT("default"), Value);
			return Schema;
		}

		static TSharedRef<FJsonObject> WithDefaultNumber(
			const TSharedRef<FJsonObject>& Schema,
			const double Value)
		{
			Schema->SetNumberField(TEXT("default"), Value);
			return Schema;
		}

		static TSharedRef<FJsonObject> WithDeprecated(
			const TSharedRef<FJsonObject>& Schema,
			const bool bDeprecated,
			const FString& Replacement = FString())
		{
			Schema->SetBoolField(TEXT("deprecated"), bDeprecated);
			if (!Replacement.IsEmpty())
			{
				Schema->SetStringField(TEXT("x-replacement"), Replacement);
			}
			return Schema;
		}

		static TSharedRef<FJsonObject> WithDefinitions(
			const TSharedRef<FJsonObject>& Schema,
			const TMap<FString, TSharedRef<FJsonObject>>& Definitions)
		{
			TSharedRef<FJsonObject> DefinitionsJson = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedRef<FJsonObject>>& Pair : Definitions)
			{
				DefinitionsJson->SetObjectField(Pair.Key, Pair.Value);
			}
			Schema->SetObjectField(TEXT("$defs"), DefinitionsJson);
			return Schema;
		}

		static TSharedRef<FJsonObject> WithAllOf(
			const TSharedRef<FJsonObject>& Schema,
			const TArray<TSharedRef<FJsonObject>>& Schemas)
		{
			Schema->SetArrayField(TEXT("allOf"), ToJsonArray(Schemas));
			return Schema;
		}

		// ------------------------------------------------------------------
		// GeoTerrain v1.1 additions (worldforge_geoterrain_v1/03_MCP_CONTRACTS.md
		// section 2): keyword-complete fragments the frozen machine registry
		// (MCP_TOOL_SCHEMAS_V1_1.json) requires. Additive only; existing helpers
		// keep their exact behavior.
		// ------------------------------------------------------------------

		/** Schema with no keywords; base for keyword-only fragments such as
		 *  {"required": [...]} or {"properties": {...}} used inside oneOf/if/not. */
		static TSharedRef<FJsonObject> Empty()
		{
			return MakeShared<FJsonObject>();
		}

		/** enum on a schema fragment that carries no explicit type. */
		static TSharedRef<FJsonObject> WithEnum(
			const TSharedRef<FJsonObject>& Schema,
			const TArray<FString>& EnumValues)
		{
			TArray<TSharedPtr<FJsonValue>> EnumJson;
			for (const FString& Value : EnumValues)
			{
				EnumJson.Add(MakeShared<FJsonValueString>(Value));
			}
			Schema->SetArrayField(TEXT("enum"), EnumJson);
			return Schema;
		}

		/** Numeric const (covers integer consts such as "const": 1). */
		static TSharedRef<FJsonObject> WithConstNumber(
			const TSharedRef<FJsonObject>& Schema,
			const double Value)
		{
			Schema->SetNumberField(TEXT("const"), Value);
			return Schema;
		}

		/** required on a schema fragment without a full object type. */
		static TSharedRef<FJsonObject> WithRequired(
			const TSharedRef<FJsonObject>& Schema,
			const TArray<FString>& Required)
		{
			TArray<TSharedPtr<FJsonValue>> RequiredJson;
			for (const FString& Field : Required)
			{
				RequiredJson.Add(MakeShared<FJsonValueString>(Field));
			}
			Schema->SetArrayField(TEXT("required"), RequiredJson);
			return Schema;
		}

		/** properties on a schema fragment without a full object type. */
		static TSharedRef<FJsonObject> WithProperties(
			const TSharedRef<FJsonObject>& Schema,
			const TMap<FString, TSharedRef<FJsonObject>>& Properties)
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedRef<FJsonObject>>& Pair : Properties)
			{
				Props->SetObjectField(Pair.Key, Pair.Value);
			}
			Schema->SetObjectField(TEXT("properties"), Props);
			return Schema;
		}

		/** not as a mutator so it can combine with required/properties in one fragment. */
		static TSharedRef<FJsonObject> WithNot(
			const TSharedRef<FJsonObject>& Schema,
			const TSharedRef<FJsonObject>& SubSchema)
		{
			Schema->SetObjectField(TEXT("not"), SubSchema);
			return Schema;
		}

		static TSharedRef<FJsonObject> WithOneOf(
			const TSharedRef<FJsonObject>& Schema,
			const TArray<TSharedRef<FJsonObject>>& Schemas)
		{
			Schema->SetArrayField(TEXT("oneOf"), ToJsonArray(Schemas));
			return Schema;
		}

		static TSharedRef<FJsonObject> WithAnyOf(
			const TSharedRef<FJsonObject>& Schema,
			const TArray<TSharedRef<FJsonObject>>& Schemas)
		{
			Schema->SetArrayField(TEXT("anyOf"), ToJsonArray(Schemas));
			return Schema;
		}

	private:
		static void SetDescription(
			const TSharedRef<FJsonObject>& Schema,
			const FString& Description)
		{
			if (!Description.IsEmpty())
			{
				Schema->SetStringField(TEXT("description"), Description);
			}
		}

		static void SetNumericBounds(
			const TSharedRef<FJsonObject>& Schema,
			const TOptional<double>& Minimum,
			const TOptional<double>& Maximum,
			const bool bExclusiveMinimum,
			const bool bExclusiveMaximum)
		{
			if (Minimum.IsSet())
			{
				Schema->SetNumberField(
					bExclusiveMinimum ? TEXT("exclusiveMinimum") : TEXT("minimum"),
					Minimum.GetValue());
			}
			if (Maximum.IsSet())
			{
				Schema->SetNumberField(
					bExclusiveMaximum ? TEXT("exclusiveMaximum") : TEXT("maximum"),
					Maximum.GetValue());
			}
		}

		static TArray<TSharedPtr<FJsonValue>> ToJsonArray(
			const TArray<TSharedRef<FJsonObject>>& Schemas)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(Schemas.Num());
			for (const TSharedRef<FJsonObject>& Schema : Schemas)
			{
				Values.Add(MakeShared<FJsonValueObject>(Schema));
			}
			return Values;
		}

		static TSharedRef<FJsonObject> Composite(
			const TCHAR* Keyword,
			const TArray<TSharedRef<FJsonObject>>& Schemas)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(Keyword, ToJsonArray(Schemas));
			return Result;
		}
	};
}
