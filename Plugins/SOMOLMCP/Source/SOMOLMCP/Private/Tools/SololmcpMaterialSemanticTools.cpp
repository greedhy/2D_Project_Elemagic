// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP - Material semantic graph inspection tools.
//
// Read-only helpers kept outside SololmcpDomainTools.cpp so the large legacy
// material CRUD file does not grow further. These tools load a material asset,
// inspect the in-memory UMaterial expression graph, and return structured JSON.

#include "Tools/SololmcpToolRegistry.h"

#include "SololmcpSchemaBuilder.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInstance.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP
{
namespace MaterialSemanticTools
{
	static const TArray<TPair<FString, EMaterialProperty>>& SupportedProperties()
	{
		static const TArray<TPair<FString, EMaterialProperty>> Properties = {
			{TEXT("BaseColor"), MP_BaseColor},
			{TEXT("Metallic"), MP_Metallic},
			{TEXT("Specular"), MP_Specular},
			{TEXT("Roughness"), MP_Roughness},
			{TEXT("EmissiveColor"), MP_EmissiveColor},
			{TEXT("Opacity"), MP_Opacity},
			{TEXT("OpacityMask"), MP_OpacityMask},
			{TEXT("Normal"), MP_Normal},
			{TEXT("WorldPositionOffset"), MP_WorldPositionOffset},
			{TEXT("AmbientOcclusion"), MP_AmbientOcclusion},
			{TEXT("Refraction"), MP_Refraction},
			{TEXT("Anisotropy"), MP_Anisotropy},
			{TEXT("Tangent"), MP_Tangent},
			{TEXT("Displacement"), MP_Displacement},
			{TEXT("SubsurfaceColor"), MP_SubsurfaceColor},
			{TEXT("ClearCoat"), MP_CustomData0},
			{TEXT("ClearCoatRoughness"), MP_CustomData1},
			{TEXT("PixelDepthOffset"), MP_PixelDepthOffset}
		};
		return Properties;
	}

	static bool TryParseMaterialProperty(const FString& RawName, FString& OutCanonicalName, EMaterialProperty& OutProperty, const UMaterial* Material = nullptr)
	{
		FString Stripped = RawName.TrimStartAndEnd();
		Stripped.RemoveFromStart(TEXT("MP_"), ESearchCase::IgnoreCase);
		const FString Normalized = Stripped.ToLower().Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
		static const TMap<FString, FString> Aliases = {
			{TEXT("basecolour"), TEXT("BaseColor")},
			{TEXT("basecolor"), TEXT("BaseColor")},
			{TEXT("emissive"), TEXT("EmissiveColor")},
			{TEXT("emissivecolor"), TEXT("EmissiveColor")},
			{TEXT("wpo"), TEXT("WorldPositionOffset")},
			{TEXT("worldpositionoffset"), TEXT("WorldPositionOffset")},
			{TEXT("ao"), TEXT("AmbientOcclusion")},
			{TEXT("ambientocclusion"), TEXT("AmbientOcclusion")},
			{TEXT("clearcoat"), TEXT("ClearCoat")},
			{TEXT("clearcoatroughness"), TEXT("ClearCoatRoughness")},
			{TEXT("pdo"), TEXT("PixelDepthOffset")},
			{TEXT("pixeldepthoffset"), TEXT("PixelDepthOffset")}
		};

		for (const TPair<FString, EMaterialProperty>& Pair : SupportedProperties())
		{
			if (Pair.Key.ToLower() == Normalized || Pair.Key.ToLower().Replace(TEXT("_"), TEXT("")) == Normalized)
			{
				OutCanonicalName = Pair.Key;
				OutProperty = Pair.Value;
				return true;
			}
		}

		if (const FString* Canonical = Aliases.Find(Normalized))
		{
			for (const TPair<FString, EMaterialProperty>& Pair : SupportedProperties())
			{
				if (Pair.Key == *Canonical)
				{
					OutCanonicalName = Pair.Key;
					OutProperty = Pair.Value;
					return true;
				}
			}
		}

		// Domain-specific pin aliases. UE 5.8 has no MP_Extinction enum value:
		// on MD_Volume materials the volume "Extinction" pin is MP_SubsurfaceColor
		// and "Albedo" is MP_BaseColor (FMaterialAttributeDefinitionMap overrides).
		if (Material && Material->MaterialDomain == MD_Volume)
		{
			if (Normalized == TEXT("extinction"))
			{
				OutCanonicalName = TEXT("Extinction");
				OutProperty = MP_SubsurfaceColor;
				return true;
			}
			if (Normalized == TEXT("albedo"))
			{
				OutCanonicalName = TEXT("Albedo");
				OutProperty = MP_BaseColor;
				return true;
			}
		}

		// Generic fallback: any other EMaterialProperty enum name active for the
		// material's domain (PostProcess/DeferredDecal/UI/etc.).
		if (Material)
		{
			if (const UEnum* PropertyEnum = StaticEnum<EMaterialProperty>())
			{
				for (int32 Index = 0; Index < PropertyEnum->NumEnums(); ++Index)
				{
					const int64 Value = PropertyEnum->GetValueByIndex(Index);
					if (Value == MP_MAX || Value == MP_MaterialAttributes || Value == MP_CustomOutput)
					{
						continue;
					}
					FString EnumName = PropertyEnum->GetNameStringByIndex(Index);
					EnumName.RemoveFromStart(TEXT("MP_"), ESearchCase::IgnoreCase);
					if (EnumName.ToLower().Replace(TEXT("_"), TEXT("")) == Normalized
						&& Material->IsPropertyActiveInEditor(static_cast<EMaterialProperty>(Value)))
					{
						OutCanonicalName = EnumName;
						OutProperty = static_cast<EMaterialProperty>(Value);
						return true;
					}
				}
			}
		}
		return false;
	}

	static int32 FindExpressionIndex(const UMaterial* Material, const UMaterialExpression* Expression)
	{
		if (!Material || !Expression)
		{
			return INDEX_NONE;
		}

		const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();
		for (int32 Index = 0; Index < Expressions.Num(); ++Index)
		{
			if (Expressions[Index] == Expression)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	static FString OutputNameForIndex(UMaterialExpression* Expression, const int32 OutputIndex)
	{
		if (!Expression || OutputIndex == INDEX_NONE)
		{
			return FString();
		}

		const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
		if (Outputs.IsValidIndex(OutputIndex))
		{
			return Outputs[OutputIndex].OutputName.ToString();
		}
		return FString();
	}

	static FString ShortClassName(const UObject* Object)
	{
		if (!Object || !Object->GetClass())
		{
			return FString();
		}
		FString Name = Object->GetClass()->GetName();
		Name.RemoveFromStart(TEXT("MaterialExpression"));
		return Name;
	}

	static FString StageRoleForExpression(const UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return TEXT("unknown");
		}

		const FString ClassName = Expression->GetClass()->GetName();
		if (ClassName.Contains(TEXT("Parameter")))
		{
			return TEXT("parameter");
		}
		if (ClassName.Contains(TEXT("Texture")))
		{
			return TEXT("texture");
		}
		if (ClassName.Contains(TEXT("Constant")) || ClassName.Contains(TEXT("Time")) ||
			ClassName.Contains(TEXT("WorldPosition")) || ClassName.Contains(TEXT("CameraVector")) ||
			ClassName.Contains(TEXT("VertexNormal")))
		{
			return TEXT("source");
		}
		if (ClassName.Contains(TEXT("Add")) || ClassName.Contains(TEXT("Multiply")) ||
			ClassName.Contains(TEXT("Subtract")) || ClassName.Contains(TEXT("LinearInterpolate")) ||
			ClassName.Contains(TEXT("Lerp")) || ClassName.Contains(TEXT("Clamp")) ||
			ClassName.Contains(TEXT("Power")) || ClassName.Contains(TEXT("Saturate")) ||
			ClassName.Contains(TEXT("OneMinus")) || ClassName.Contains(TEXT("ComponentMask")))
		{
			return TEXT("transform");
		}
		return TEXT("expression");
	}

	static TSharedRef<FJsonObject> MaterialExpressionClassToJson(UClass* Class, const bool bIncludeSchema)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Class)
		{
			return Result;
		}
		Result->SetStringField(TEXT("class_path"), Class->GetPathName());
		Result->SetStringField(TEXT("class_name"), Class->GetName());
		Result->SetStringField(TEXT("display_name"), Class->GetDisplayNameText().ToString());
		Result->SetBoolField(TEXT("abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
		Result->SetBoolField(TEXT("deprecated"), Class->HasAnyClassFlags(CLASS_Deprecated));
		Result->SetBoolField(TEXT("transient"), Class->HasAnyClassFlags(CLASS_Transient));
#if WITH_METADATA
		Result->SetStringField(TEXT("category"), Class->GetMetaData(TEXT("Category")));
		Result->SetStringField(TEXT("tooltip"), Class->GetToolTipText().ToString());
#endif

		if (!bIncludeSchema)
		{
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> Properties;
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				continue;
			}
			TSharedRef<FJsonObject> PropertyJson = MakeShared<FJsonObject>();
			PropertyJson->SetStringField(TEXT("name"), Property->GetName());
			PropertyJson->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			PropertyJson->SetStringField(TEXT("owner_class"), Property->GetOwnerClass() ? Property->GetOwnerClass()->GetPathName() : FString());
			PropertyJson->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
			PropertyJson->SetBoolField(TEXT("blueprint_visible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
#if WITH_METADATA
			PropertyJson->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
			PropertyJson->SetStringField(TEXT("tooltip"), Property->GetToolTipText().ToString());
			PropertyJson->SetStringField(TEXT("clamp_min"), Property->GetMetaData(TEXT("ClampMin")));
			PropertyJson->SetStringField(TEXT("clamp_max"), Property->GetMetaData(TEXT("ClampMax")));
			PropertyJson->SetStringField(TEXT("ui_min"), Property->GetMetaData(TEXT("UIMin")));
			PropertyJson->SetStringField(TEXT("ui_max"), Property->GetMetaData(TEXT("UIMax")));
#endif
			Properties.Add(MakeShared<FJsonValueObject>(PropertyJson));
		}
		Result->SetArrayField(TEXT("properties"), Properties);

		TArray<TSharedPtr<FJsonValue>> Inputs;
		TArray<TSharedPtr<FJsonValue>> Outputs;
		if (!Class->HasAnyClassFlags(CLASS_Abstract))
		{
			if (UMaterialExpression* Cdo = Cast<UMaterialExpression>(Class->GetDefaultObject()))
			{
				for (int32 Index = 0; ; ++Index)
				{
					if (!Cdo->GetInput(Index)) { break; }
					TSharedRef<FJsonObject> InputJson = MakeShared<FJsonObject>();
					InputJson->SetNumberField(TEXT("index"), Index);
					InputJson->SetStringField(TEXT("name"), Cdo->GetInputName(Index).ToString());
					Inputs.Add(MakeShared<FJsonValueObject>(InputJson));
				}
				const TArray<FExpressionOutput>& ClassOutputs = Cdo->GetOutputs();
				for (int32 Index = 0; Index < ClassOutputs.Num(); ++Index)
				{
					TSharedRef<FJsonObject> OutputJson = MakeShared<FJsonObject>();
					OutputJson->SetNumberField(TEXT("index"), Index);
					OutputJson->SetStringField(TEXT("name"), ClassOutputs[Index].OutputName.ToString());
					OutputJson->SetBoolField(TEXT("mask_r"), ClassOutputs[Index].MaskR != 0);
					OutputJson->SetBoolField(TEXT("mask_g"), ClassOutputs[Index].MaskG != 0);
					OutputJson->SetBoolField(TEXT("mask_b"), ClassOutputs[Index].MaskB != 0);
					OutputJson->SetBoolField(TEXT("mask_a"), ClassOutputs[Index].MaskA != 0);
					Outputs.Add(MakeShared<FJsonValueObject>(OutputJson));
				}
			}
		}
		Result->SetArrayField(TEXT("inputs"), Inputs);
		Result->SetArrayField(TEXT("outputs"), Outputs);
		return Result;
	}

	static TSharedRef<FJsonObject> ExpressionRefToJson(const UMaterial* Material, UMaterialExpression* Expression)
	{
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetNumberField(TEXT("index"), FindExpressionIndex(Material, Expression));
		Node->SetStringField(TEXT("guid"), Expression ? Expression->MaterialExpressionGuid.ToString() : FString());
		Node->SetStringField(TEXT("name"), Expression ? Expression->GetName() : FString());
		Node->SetStringField(TEXT("class"), Expression && Expression->GetClass() ? Expression->GetClass()->GetPathName() : FString());
		Node->SetStringField(TEXT("class_short"), ShortClassName(Expression));
		Node->SetStringField(TEXT("stage_role"), StageRoleForExpression(Expression));
		Node->SetStringField(TEXT("parameter_name"), Expression ? Expression->GetParameterName().ToString() : FString());
		Node->SetNumberField(TEXT("x"), Expression ? Expression->MaterialExpressionEditorX : 0);
		Node->SetNumberField(TEXT("y"), Expression ? Expression->MaterialExpressionEditorY : 0);
		return Node;
	}

	static void AddExpressionInputEdges(
		UMaterial* Material,
		UMaterialExpression* ToExpression,
		TArray<TSharedPtr<FJsonValue>>& OutEdges,
		TArray<TSharedPtr<FJsonValue>>* OutInputs = nullptr)
	{
		if (!Material || !ToExpression)
		{
			return;
		}

		for (int32 InputIndex = 0; ; ++InputIndex)
		{
			FExpressionInput* Input = ToExpression->GetInput(InputIndex);
			if (!Input)
			{
				break;
			}
			UMaterialExpression* FromExpression = Input ? Input->Expression : nullptr;
			if (!FromExpression)
			{
				continue;
			}

			const FString InputName = ToExpression->GetInputName(InputIndex).ToString();
			const FString OutputName = OutputNameForIndex(FromExpression, Input->OutputIndex);
			TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
			Edge->SetStringField(TEXT("kind"), TEXT("expression_input"));
			Edge->SetNumberField(TEXT("from_index"), FindExpressionIndex(Material, FromExpression));
			Edge->SetStringField(TEXT("from_guid"), FromExpression->MaterialExpressionGuid.ToString());
			Edge->SetStringField(TEXT("from_output"), OutputName);
			Edge->SetNumberField(TEXT("to_index"), FindExpressionIndex(Material, ToExpression));
			Edge->SetStringField(TEXT("to_guid"), ToExpression->MaterialExpressionGuid.ToString());
			Edge->SetStringField(TEXT("to_input"), InputName);
			OutEdges.Add(MakeShared<FJsonValueObject>(Edge));

			if (OutInputs)
			{
				TSharedRef<FJsonObject> InputJson = MakeShared<FJsonObject>();
				InputJson->SetStringField(TEXT("input"), InputName);
				InputJson->SetStringField(TEXT("from_output"), OutputName);
				InputJson->SetObjectField(TEXT("from"), ExpressionRefToJson(Material, FromExpression));
				OutInputs->Add(MakeShared<FJsonValueObject>(InputJson));
			}
		}
	}

	static bool LoadGraphMaterial(
		FSololmcpEditorServices& Services,
		const FString& AssetPath,
		UMaterial*& OutMaterial,
		FString& OutGraphAssetPath,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		UObject* Asset = Services.LoadAsset(AssetPath, OutError);
		if (!Asset)
		{
			return false;
		}

		if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			OutMaterial = Material;
			OutGraphAssetPath = Material->GetPathName();
			return true;
		}

		if (UMaterialInstance* Instance = Cast<UMaterialInstance>(Asset))
		{
			if (UMaterialInterface* Parent = Instance->Parent)
			{
				if (UMaterial* ParentMaterial = Cast<UMaterial>(Parent))
				{
					OutMaterial = ParentMaterial;
					OutGraphAssetPath = ParentMaterial->GetPathName();
					OutStructured->SetStringField(TEXT("inspected_instance"), Asset->GetPathName());
					OutStructured->SetStringField(TEXT("resolved_parent_material"), OutGraphAssetPath);
					return true;
				}
			}
			OutError = FString::Printf(TEXT("Material instance has no UMaterial parent graph: %s"), *AssetPath);
			return false;
		}

		OutError = FString::Printf(TEXT("Asset is not a UMaterial or UMaterialInstance (got %s) at %s"),
			*Asset->GetClass()->GetName(), *AssetPath);
		return false;
	}

	static void TraceExpression(
		UMaterial* Material,
		UMaterialExpression* Expression,
		const int32 Depth,
		const int32 MaxDepth,
		TSet<UMaterialExpression*>& Visiting,
		TSet<UMaterialExpression*>& Seen,
		TArray<TSharedPtr<FJsonValue>>& OutNodes,
		TArray<TSharedPtr<FJsonValue>>& OutEdges,
		TArray<TSharedPtr<FJsonValue>>& OutWarnings)
	{
		if (!Material || !Expression)
		{
			return;
		}

		if (Depth > MaxDepth)
		{
			TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
			Warning->SetStringField(TEXT("code"), TEXT("max_depth_reached"));
			Warning->SetNumberField(TEXT("expression_index"), FindExpressionIndex(Material, Expression));
			Warning->SetNumberField(TEXT("max_depth"), MaxDepth);
			OutWarnings.Add(MakeShared<FJsonValueObject>(Warning));
			return;
		}

		if (Visiting.Contains(Expression))
		{
			TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
			Warning->SetStringField(TEXT("code"), TEXT("cycle_detected"));
			Warning->SetNumberField(TEXT("expression_index"), FindExpressionIndex(Material, Expression));
			OutWarnings.Add(MakeShared<FJsonValueObject>(Warning));
			return;
		}

		if (!Seen.Contains(Expression))
		{
			TSharedRef<FJsonObject> Node = ExpressionRefToJson(Material, Expression);
			Node->SetNumberField(TEXT("trace_depth"), Depth);
			OutNodes.Add(MakeShared<FJsonValueObject>(Node));
			Seen.Add(Expression);
		}

		Visiting.Add(Expression);
		AddExpressionInputEdges(Material, Expression, OutEdges);

		for (int32 InputIndex = 0; ; ++InputIndex)
		{
			FExpressionInput* Input = Expression->GetInput(InputIndex);
			if (!Input)
			{
				break;
			}
			if (Input && Input->Expression)
			{
				TraceExpression(Material, Input->Expression, Depth + 1, MaxDepth, Visiting, Seen, OutNodes, OutEdges, OutWarnings);
			}
		}
		Visiting.Remove(Expression);
	}

	static TSharedRef<FJsonObject> PropertyConnectionToJson(UMaterial* Material, const FString& PropertyName, const EMaterialProperty Property)
	{
		TSharedRef<FJsonObject> PropertyJson = MakeShared<FJsonObject>();
		PropertyJson->SetStringField(TEXT("property"), PropertyName);
		PropertyJson->SetBoolField(TEXT("connected"), Material ? Material->IsPropertyConnected(Property) : false);

		FExpressionInput* Input = Material ? Material->GetExpressionInputForProperty(Property) : nullptr;
		UMaterialExpression* Expression = Input ? Input->Expression : nullptr;
		if (Expression)
		{
			PropertyJson->SetObjectField(TEXT("root"), ExpressionRefToJson(Material, Expression));
			PropertyJson->SetStringField(TEXT("root_output"), OutputNameForIndex(Expression, Input->OutputIndex));
		}
		return PropertyJson;
	}

	static FString BuildNarrative(UMaterial* Material, const TArray<TSharedPtr<FJsonValue>>& ConnectedProperties)
	{
		if (!Material)
		{
			return FString();
		}

		TArray<FString> Parts;
		for (const TSharedPtr<FJsonValue>& Value : ConnectedProperties)
		{
			const TSharedPtr<FJsonObject> Property = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Property.IsValid())
			{
				continue;
			}
			FString PropertyName;
			if (!Property->TryGetStringField(TEXT("property"), PropertyName))
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* RootPtr = nullptr;
			if (Property->TryGetObjectField(TEXT("root"), RootPtr) && RootPtr && RootPtr->IsValid())
			{
				FString ClassShort;
				(*RootPtr)->TryGetStringField(TEXT("class_short"), ClassShort);
				FString ParameterName;
				(*RootPtr)->TryGetStringField(TEXT("parameter_name"), ParameterName);
				if (!ParameterName.IsEmpty())
				{
					Parts.Add(FString::Printf(TEXT("%s is driven by %s parameter '%s'"), *PropertyName, *ClassShort, *ParameterName));
				}
				else
				{
					Parts.Add(FString::Printf(TEXT("%s is driven by %s"), *PropertyName, *ClassShort));
				}
			}
		}

		if (Parts.IsEmpty())
		{
			return TEXT("No supported material properties are connected.");
		}
		return FString::Join(Parts, TEXT("; ")) + TEXT(".");
	}
}

void RegisterMaterialSemanticTools(FSololmcpToolRegistry& Registry)
{
	using namespace MaterialSemanticTools;
	using SB = FSololmcpSchemaBuilder;

	Registry.Register({
		TEXT("material_expression_catalog"),
		TEXT("List every loaded concrete UMaterialExpression class with searchable metadata. This is the authoritative discovery surface before generic material_create_expression/material_safe_patch writes."),
		SB::Object({
			{TEXT("query"), SB::String(TEXT("Optional case-insensitive class/display/category filter."))},
			{TEXT("offset"), SB::Integer(TEXT("Result offset; default 0."))},
			{TEXT("max_results"), SB::Integer(TEXT("Maximum results; default 200, hard cap 1000."))},
			{TEXT("include_abstract"), SB::Boolean(TEXT("Include abstract expression classes; default false."))}
		}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			FString Query;
			Arguments->TryGetStringField(TEXT("query"), Query);
			const FString QueryLower = Query.TrimStartAndEnd().ToLower();
			const int32 Offset = Arguments->HasTypedField<EJson::Number>(TEXT("offset")) ? FMath::Max(0, Arguments->GetIntegerField(TEXT("offset"))) : 0;
			const int32 MaxResults = Arguments->HasTypedField<EJson::Number>(TEXT("max_results")) ? FMath::Clamp(Arguments->GetIntegerField(TEXT("max_results")), 1, 1000) : 200;
			const bool bIncludeAbstract = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_abstract")) && Arguments->GetBoolField(TEXT("include_abstract"));

			TArray<UClass*> Classes;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (!Class || !Class->IsChildOf(UMaterialExpression::StaticClass()) || Class == UMaterialExpression::StaticClass()) { continue; }
				if (!bIncludeAbstract && Class->HasAnyClassFlags(CLASS_Abstract)) { continue; }
				const FString Search = (Class->GetPathName() + TEXT(" ") + Class->GetDisplayNameText().ToString()).ToLower();
				if (!QueryLower.IsEmpty() && !Search.Contains(QueryLower)) { continue; }
				Classes.Add(Class);
			}
			Classes.Sort([](const UClass& A, const UClass& B) { return A.GetPathName() < B.GetPathName(); });

			TArray<TSharedPtr<FJsonValue>> Results;
			for (int32 Index = Offset; Index < Classes.Num() && Results.Num() < MaxResults; ++Index)
			{
				Results.Add(MakeShared<FJsonValueObject>(MaterialExpressionClassToJson(Classes[Index], false)));
			}
			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.material.expression_catalog.v1"));
			OutStructured->SetNumberField(TEXT("matched_count"), Classes.Num());
			OutStructured->SetNumberField(TEXT("offset"), Offset);
			OutStructured->SetNumberField(TEXT("returned_count"), Results.Num());
			OutStructured->SetArrayField(TEXT("classes"), Results);
			OutSummary = FString::Printf(TEXT("Found %d material expression classes; returned %d."), Classes.Num(), Results.Num());
			return true;
		},
		nullptr,
		60
	});

	Registry.Register({
		TEXT("material_expression_schema_inspect"),
		TEXT("Inspect one UMaterialExpression class, including editable reflected properties, categories, ranges, inputs, and outputs. Read this schema before creating or modifying a material node."),
		SB::Object({{TEXT("class_path"), SB::String(TEXT("Full UMaterialExpression class path or class name."))}}, {TEXT("class_path")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString ClassPath;
			Arguments->TryGetStringField(TEXT("class_path"), ClassPath);
			UClass* Match = LoadObject<UClass>(nullptr, *ClassPath);
			if (!Match)
			{
				for (TObjectIterator<UClass> It; It; ++It)
				{
					if (It->GetName().Equals(ClassPath, ESearchCase::IgnoreCase) || It->GetPathName().Equals(ClassPath, ESearchCase::IgnoreCase)) { Match = *It; break; }
				}
			}
			if (!Match || !Match->IsChildOf(UMaterialExpression::StaticClass()))
			{
				OutError = FString::Printf(TEXT("Material expression class was not found: %s"), *ClassPath);
				return false;
			}
			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.material.expression_class_schema.v1"));
			OutStructured->SetObjectField(TEXT("expression_class"), MaterialExpressionClassToJson(Match, true));
			OutStructured->SetStringField(TEXT("writer_tool"), TEXT("material_create_expression or material_safe_patch(create_expression)"));
			OutSummary = FString::Printf(TEXT("Inspected material expression schema %s."), *Match->GetPathName());
			return true;
		},
		nullptr,
		60
	});

	Registry.Register({
		TEXT("material_graph_explain"),
		TEXT("Explain a material graph as semantic nodes, expression edges, connected material properties, and a short narrative. Read-only."),
		SB::Object({
			{TEXT("asset_path"), SB::String(TEXT("UMaterial path, or a MaterialInstance whose parent is a UMaterial"))},
			{TEXT("include_disconnected"), SB::Boolean(TEXT("Include disconnected expression nodes; default true"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				OutError = TEXT("Missing asset_path.");
				return false;
			}

			UMaterial* Material = nullptr;
			FString GraphAssetPath;
			if (!LoadGraphMaterial(Context.Services, AssetPath, Material, GraphAssetPath, OutStructured, OutError))
			{
				return false;
			}

			const bool bIncludeDisconnected = !Arguments->HasTypedField<EJson::Boolean>(TEXT("include_disconnected")) ||
				Arguments->GetBoolField(TEXT("include_disconnected"));

			TArray<TSharedPtr<FJsonValue>> PropertiesJson;
			TSet<UMaterialExpression*> ConnectedRoots;
			for (const TPair<FString, EMaterialProperty>& Pair : SupportedProperties())
			{
				TSharedRef<FJsonObject> PropertyJson = PropertyConnectionToJson(Material, Pair.Key, Pair.Value);
				const bool bConnected = PropertyJson->GetBoolField(TEXT("connected"));
				if (bConnected)
				{
					const FExpressionInput* Input = Material->GetExpressionInputForProperty(Pair.Value);
					if (Input && Input->Expression)
					{
						ConnectedRoots.Add(Input->Expression);
					}
				}
				PropertiesJson.Add(MakeShared<FJsonValueObject>(PropertyJson));
			}

			TArray<TSharedPtr<FJsonValue>> NodesJson;
			TArray<TSharedPtr<FJsonValue>> EdgesJson;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (!Expression)
				{
					continue;
				}

				TArray<TSharedPtr<FJsonValue>> InputRefs;
				AddExpressionInputEdges(Material, Expression, EdgesJson, &InputRefs);

				if (bIncludeDisconnected || ConnectedRoots.Contains(Expression) || InputRefs.Num() > 0)
				{
					TSharedRef<FJsonObject> Node = ExpressionRefToJson(Material, Expression);
					Node->SetArrayField(TEXT("inputs"), InputRefs);
					NodesJson.Add(MakeShared<FJsonValueObject>(Node));
				}
			}

			for (const TPair<FString, EMaterialProperty>& Pair : SupportedProperties())
			{
				FExpressionInput* Input = Material->GetExpressionInputForProperty(Pair.Value);
				if (!Input || !Input->Expression)
				{
					continue;
				}

				TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
				Edge->SetStringField(TEXT("kind"), TEXT("material_property"));
				Edge->SetNumberField(TEXT("from_index"), FindExpressionIndex(Material, Input->Expression));
				Edge->SetStringField(TEXT("from_guid"), Input->Expression->MaterialExpressionGuid.ToString());
				Edge->SetStringField(TEXT("from_output"), OutputNameForIndex(Input->Expression, Input->OutputIndex));
				Edge->SetStringField(TEXT("to_property"), Pair.Key);
				EdgesJson.Add(MakeShared<FJsonValueObject>(Edge));
			}

			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("graph_asset_path"), GraphAssetPath);
			OutStructured->SetStringField(TEXT("material_name"), Material->GetName());
			OutStructured->SetNumberField(TEXT("expression_count"), Material->GetExpressions().Num());
			OutStructured->SetArrayField(TEXT("properties"), PropertiesJson);
			OutStructured->SetArrayField(TEXT("nodes"), NodesJson);
			OutStructured->SetArrayField(TEXT("edges"), EdgesJson);
			OutStructured->SetStringField(TEXT("narrative"), BuildNarrative(Material, PropertiesJson));
			OutSummary = FString::Printf(TEXT("Explained material graph %s (%d expressions, %d edges)."),
				*GraphAssetPath, Material->GetExpressions().Num(), EdgesJson.Num());
			return true;
		},
		nullptr,
		30
	});

	Registry.Register({
		TEXT("material_property_trace"),
		TEXT("Trace one material property upstream through expression inputs. Read-only; useful for QA and semantic handoff receipts."),
		SB::Object({
			{TEXT("asset_path"), SB::String(TEXT("UMaterial path, or a MaterialInstance whose parent is a UMaterial"))},
			{TEXT("property"), SB::String(TEXT("Material property name, e.g. BaseColor, Roughness, Normal, EmissiveColor"))},
			{TEXT("max_depth"), SB::Integer(TEXT("Maximum upstream traversal depth; default 32"))}
		}, {TEXT("asset_path"), TEXT("property")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			FString PropertyName;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
				!Arguments->TryGetStringField(TEXT("property"), PropertyName))
			{
				OutError = TEXT("Missing asset_path or property.");
				return false;
			}

			UMaterial* Material = nullptr;
			FString GraphAssetPath;
			if (!LoadGraphMaterial(Context.Services, AssetPath, Material, GraphAssetPath, OutStructured, OutError))
			{
				return false;
			}

			FString CanonicalPropertyName;
			EMaterialProperty Property = MP_BaseColor;
			if (!TryParseMaterialProperty(PropertyName, CanonicalPropertyName, Property, Material))
			{
				OutError = FString::Printf(TEXT("Unsupported material property: %s"), *PropertyName);
				return false;
			}

			const int32 MaxDepth = Arguments->HasTypedField<EJson::Number>(TEXT("max_depth"))
				? FMath::Clamp(Arguments->GetIntegerField(TEXT("max_depth")), 0, 128)
				: 32;

			FExpressionInput* PropertyInput = Material->GetExpressionInputForProperty(Property);
			UMaterialExpression* RootExpression = PropertyInput ? PropertyInput->Expression : nullptr;

			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("graph_asset_path"), GraphAssetPath);
			OutStructured->SetStringField(TEXT("property"), CanonicalPropertyName);
			OutStructured->SetBoolField(TEXT("connected"), RootExpression != nullptr);
			OutStructured->SetNumberField(TEXT("max_depth"), MaxDepth);

			if (!RootExpression)
			{
				TArray<TSharedPtr<FJsonValue>> EmptyArray;
				OutStructured->SetArrayField(TEXT("nodes"), EmptyArray);
				OutStructured->SetArrayField(TEXT("edges"), EmptyArray);
				OutStructured->SetArrayField(TEXT("warnings"), EmptyArray);
				OutSummary = FString::Printf(TEXT("%s is not connected on %s."), *CanonicalPropertyName, *GraphAssetPath);
				return true;
			}

			TArray<TSharedPtr<FJsonValue>> NodesJson;
			TArray<TSharedPtr<FJsonValue>> EdgesJson;
			TArray<TSharedPtr<FJsonValue>> WarningsJson;
			TSet<UMaterialExpression*> Visiting;
			TSet<UMaterialExpression*> Seen;
			TraceExpression(Material, RootExpression, 0, MaxDepth, Visiting, Seen, NodesJson, EdgesJson, WarningsJson);

			TSharedRef<FJsonObject> PropertyEdge = MakeShared<FJsonObject>();
			PropertyEdge->SetStringField(TEXT("kind"), TEXT("material_property"));
			PropertyEdge->SetNumberField(TEXT("from_index"), FindExpressionIndex(Material, RootExpression));
			PropertyEdge->SetStringField(TEXT("from_guid"), RootExpression->MaterialExpressionGuid.ToString());
			PropertyEdge->SetStringField(TEXT("from_output"), OutputNameForIndex(RootExpression, PropertyInput->OutputIndex));
			PropertyEdge->SetStringField(TEXT("to_property"), CanonicalPropertyName);
			EdgesJson.Add(MakeShared<FJsonValueObject>(PropertyEdge));

			OutStructured->SetObjectField(TEXT("root"), ExpressionRefToJson(Material, RootExpression));
			OutStructured->SetStringField(TEXT("root_output"), OutputNameForIndex(RootExpression, PropertyInput->OutputIndex));
			OutStructured->SetArrayField(TEXT("nodes"), NodesJson);
			OutStructured->SetArrayField(TEXT("edges"), EdgesJson);
			OutStructured->SetArrayField(TEXT("warnings"), WarningsJson);
			OutSummary = FString::Printf(TEXT("Traced %s on %s through %d expression nodes."),
				*CanonicalPropertyName, *GraphAssetPath, NodesJson.Num());
			return true;
		},
		nullptr,
		30
	});
}
}
