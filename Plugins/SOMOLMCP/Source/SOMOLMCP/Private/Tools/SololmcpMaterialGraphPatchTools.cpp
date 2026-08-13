// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP - Material graph diff and safe patch contracts.
//
// The diff tool is read-only. material_safe_patch supports dry-run validation
// plus a hardened write subset for expression property edits and graph links.

#include "Tools/SololmcpToolRegistry.h"

#include "SololmcpSchemaBuilder.h"
#include "Dom/JsonValue.h"
#include "MaterialDomain.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Crc.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 5
// FExpressionInputIterator arrived in 5.5. It is a thin wrapper over
// UMaterialExpression::GetInput(Index), which 5.3 and 5.4 already have, so this is a
// port of the engine's own struct rather than a reimplementation -- the four loops in
// this file then read identically on every engine version.
struct FExpressionInputIterator
{
	UMaterialExpression* Expression;
	FExpressionInput* Input;
	int Index;

	explicit FExpressionInputIterator(UMaterialExpression* InExpression)
		: Expression{InExpression}
		, Input{InExpression ? InExpression->GetInput(0) : nullptr}
		, Index{0}
	{
	}

	operator bool() const { return Input != nullptr; }

	FExpressionInputIterator& operator++()
	{
		Index += 1;
		Input = Expression ? Expression->GetInput(Index) : nullptr;
		return *this;
	}

	FExpressionInput* operator->() { return Input; }
};
#endif

namespace UE::SOMOLMCP
{
namespace MaterialGraphPatchTools
{
	static constexpr const TCHAR* ContractVersion = TEXT("material.graph_patch.v1");

	static bool LoadMaterialForGraph(
		FSololmcpEditorServices& Services,
		const FString& AssetPath,
		UMaterial*& OutMaterial,
		FString& OutGraphAssetPath,
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
			if (UMaterial* ParentMaterial = Cast<UMaterial>(Instance->Parent))
			{
				OutMaterial = ParentMaterial;
				OutGraphAssetPath = ParentMaterial->GetPathName();
				return true;
			}
			OutError = FString::Printf(TEXT("Material instance has no UMaterial parent graph: %s"), *AssetPath);
			return false;
		}

		OutError = FString::Printf(
			TEXT("Asset is not a UMaterial or UMaterialInstance (got %s) at %s"),
			*Asset->GetClass()->GetName(),
			*AssetPath);
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

	static FString ExpressionId(const UMaterial* Material, const UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return TEXT("");
		}

		const FString Guid = Expression->MaterialExpressionGuid.ToString();
		if (!Guid.IsEmpty())
		{
			return Guid;
		}
		return FString::Printf(TEXT("index:%d"), FindExpressionIndex(Material, Expression));
	}

	static FString ShortClassName(const UObject* Object)
	{
		if (!Object || !Object->GetClass())
		{
			return TEXT("");
		}

		FString Name = Object->GetClass()->GetName();
		Name.RemoveFromStart(TEXT("MaterialExpression"));
		return Name;
	}

	static TSharedRef<FJsonObject> MakeEvidenceReceipt(const FString& Phase, const FString& Status, const FString& Reason)
	{
		TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(TEXT("phase"), Phase);
		Evidence->SetStringField(TEXT("status"), Status);
		Evidence->SetStringField(TEXT("reason"), Reason);
		return Evidence;
	}

	static TSharedRef<FJsonObject> MakeMaterialTargetBinding(const FString& AssetPath, const FString& GraphPath, const UMaterial* Material)
	{
		TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
		Binding->SetStringField(TEXT("asset_path"), AssetPath);
		Binding->SetStringField(TEXT("graph_asset_path"), GraphPath);
		Binding->SetStringField(TEXT("asset_class"), Material && Material->GetClass() ? Material->GetClass()->GetName() : FString());
		Binding->SetStringField(TEXT("package_path"), Material && Material->GetPackage() ? Material->GetPackage()->GetName() : FString());
		return Binding;
	}

	static TSharedRef<FJsonObject> MakeMaterialSnapshotSummary(const TSharedRef<FJsonObject>& Snapshot)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("graph_hash"), Snapshot->GetStringField(TEXT("graph_hash")));
		Summary->SetNumberField(TEXT("node_count"), Snapshot->GetIntegerField(TEXT("node_count")));
		Summary->SetNumberField(TEXT("edge_count"), Snapshot->GetIntegerField(TEXT("edge_count")));
		return Summary;
	}

	static void AttachMaterialGraphEditReceipt(
		TSharedRef<FJsonObject>& OutStructured,
		const FString& AssetPath,
		const FString& GraphPath,
		UMaterial* Material,
		const TSharedRef<FJsonObject>& BeforeSnapshot,
		const TSharedRef<FJsonObject>& AfterSnapshot,
		const FString& Operation,
		const FString& ReadbackField,
		const bool bReadbackOk)
	{
		OutStructured->SetStringField(TEXT("receipt_schema"), TEXT("somol.material_graph_edit_receipt.v1"));
		OutStructured->SetStringField(TEXT("operation"), Operation);
		OutStructured->SetObjectField(TEXT("target_binding"), MakeMaterialTargetBinding(AssetPath, GraphPath, Material));
		OutStructured->SetObjectField(TEXT("pre_edit_graph_summary"), MakeMaterialSnapshotSummary(BeforeSnapshot));
		OutStructured->SetObjectField(TEXT("graph_summary"), MakeMaterialSnapshotSummary(AfterSnapshot));
		TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
		Readback->SetStringField(TEXT("field"), ReadbackField);
		Readback->SetBoolField(TEXT("verified"), bReadbackOk);
		Readback->SetStringField(TEXT("tool"), TEXT("material_graph_snapshot_readback"));
		OutStructured->SetObjectField(TEXT("post_edit_readback"), Readback);
		OutStructured->SetStringField(TEXT("rollback_hint"), TEXT("Use material_graph_restore_snapshot with before_snapshot on disposable targets, or editor transaction/undo before saving."));
		OutStructured->SetObjectField(TEXT("compile_evidence"), MakeEvidenceReceipt(
			TEXT("compile"),
			TEXT("enforced_by_receipt_gate"),
			TEXT("Material graph write tools run material recompile/statistics/snapshot readback before completed receipts.")));
		OutStructured->SetObjectField(TEXT("preview_evidence"), MakeEvidenceReceipt(
			TEXT("preview"),
			TEXT("external_visual_proof_required"),
			TEXT("Capture material preview/thumb diff for visual acceptance when the caller needs final art proof.")));
		OutStructured->SetStringField(TEXT("diagnostic_code"), bReadbackOk ? TEXT("ok") : TEXT("post_edit_readback_failed"));
	}

	static FString OutputNameForIndex(UMaterialExpression* Expression, const int32 OutputIndex)
	{
		if (!Expression || OutputIndex == INDEX_NONE)
		{
			return TEXT("");
		}

		const TArray<FExpressionOutput> Outputs = Expression->GetOutputs();
		if (Outputs.IsValidIndex(OutputIndex))
		{
			return Outputs[OutputIndex].OutputName.ToString();
		}
		return TEXT("");
	}

	static const TArray<TPair<FString, EMaterialProperty>>& SupportedProperties()
	{
		// Legacy Surface-domain whitelist. Kept verbatim so snapshot edge capture
		// and property parsing for Surface materials behave exactly as before.
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

	// Domain-specific pin aliases. UE 5.8 has no MP_Extinction enum value: on
	// MD_Volume materials the volume "Extinction" pin is MP_SubsurfaceColor and
	// "Albedo" is MP_BaseColor (see
	// FMaterialAttributeDefinitionMap::GetAttributeOverrideForMaterialInLock).
	static TArray<TPair<FString, EMaterialProperty>> DomainAliasProperties(const UMaterial* Material)
	{
		TArray<TPair<FString, EMaterialProperty>> Aliases;
		if (Material && Material->MaterialDomain == MD_Volume)
		{
			Aliases.Add({TEXT("Extinction"), MP_SubsurfaceColor});
			Aliases.Add({TEXT("Albedo"), MP_BaseColor});
		}
		return Aliases;
	}

	static FString NormalizeMaterialPropertyName(const FString& PropertyName)
	{
		FString Normalized = PropertyName;
		Normalized.RemoveFromStart(TEXT("MP_"), ESearchCase::IgnoreCase);
		return Normalized.Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT("")).ToLower();
	}

	static bool TryParseMaterialProperty(const FString& PropertyName, EMaterialProperty& OutProperty, const UMaterial* Material = nullptr)
	{
		const FString Normalized = NormalizeMaterialPropertyName(PropertyName);
		for (const TPair<FString, EMaterialProperty>& Pair : SupportedProperties())
		{
			if (NormalizeMaterialPropertyName(Pair.Key) == Normalized)
			{
				OutProperty = Pair.Value;
				return true;
			}
		}
		for (const TPair<FString, EMaterialProperty>& Pair : DomainAliasProperties(Material))
		{
			if (NormalizeMaterialPropertyName(Pair.Key) == Normalized)
			{
				OutProperty = Pair.Value;
				return true;
			}
		}
		// Generic fallback: accept any other EMaterialProperty enum name when the
		// target material reports the property as active for its domain (covers
		// PostProcess/DeferredDecal/UI/etc. without another hardcoded table).
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
					if (NormalizeMaterialPropertyName(PropertyEnum->GetNameStringByIndex(Index)) == Normalized
						&& Material->IsPropertyActiveInEditor(static_cast<EMaterialProperty>(Value)))
					{
						OutProperty = static_cast<EMaterialProperty>(Value);
						return true;
					}
				}
			}
		}
		return false;
	}

	static FString JsonValueToImportString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::String:
			return Value->AsString();
		case EJson::Number:
		{
			const double Number = Value->AsNumber();
			if (FMath::IsNearlyEqual(Number, FMath::TruncToDouble(Number)))
			{
				return FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
			}
			return FString::SanitizeFloat(Number);
		}
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Null:
			return TEXT("None");
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
			if (Array.Num() == 3 &&
				Array[0].IsValid() && Array[0]->Type == EJson::Number &&
				Array[1].IsValid() && Array[1]->Type == EJson::Number &&
				Array[2].IsValid() && Array[2]->Type == EJson::Number)
			{
				return FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), Array[0]->AsNumber(), Array[1]->AsNumber(), Array[2]->AsNumber());
			}
			if (Array.Num() == 4 &&
				Array[0].IsValid() && Array[0]->Type == EJson::Number &&
				Array[1].IsValid() && Array[1]->Type == EJson::Number &&
				Array[2].IsValid() && Array[2]->Type == EJson::Number &&
				Array[3].IsValid() && Array[3]->Type == EJson::Number)
			{
				return FString::Printf(TEXT("(R=%f,G=%f,B=%f,A=%f)"), Array[0]->AsNumber(), Array[1]->AsNumber(), Array[2]->AsNumber(), Array[3]->AsNumber());
			}
			FString Out;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Array, Writer);
			return Out;
		}
		case EJson::Object:
		default:
		{
			FString Out;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
			return Out;
		}
		}
	}

	static FString MaterialExpressionClassPathFromType(const FString& ExpressionType)
	{
		FString ShortName = ExpressionType;
		ShortName.RemoveFromStart(TEXT("/Script/Engine."));
		ShortName.RemoveFromStart(TEXT("UMaterialExpression"));
		ShortName.RemoveFromStart(TEXT("MaterialExpression"));
		ShortName.RemoveFromStart(TEXT("U"));
		return FString::Printf(TEXT("/Script/Engine.MaterialExpression%s"), *ShortName);
	}

	static bool ResolveMaterialExpressionClass(
		FSololmcpEditorServices& Services,
		const TSharedRef<FJsonObject>& Operation,
		UClass*& OutClass,
		FString& OutClassPath,
		FString& OutError)
	{
		FString ClassPath;
		if (!Operation->TryGetStringField(TEXT("class"), ClassPath) || ClassPath.TrimStartAndEnd().IsEmpty())
		{
			FString ExpressionType;
			if (!Operation->TryGetStringField(TEXT("expression_type"), ExpressionType) || ExpressionType.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("expression_type or class is required.");
				return false;
			}
			ClassPath = MaterialExpressionClassPathFromType(ExpressionType);
		}

		UClass* ExpressionClass = Services.ResolveClass(ClassPath, OutError);
		if (!ExpressionClass)
		{
			return false;
		}
		if (!ExpressionClass->IsChildOf(UMaterialExpression::StaticClass()))
		{
			OutError = FString::Printf(TEXT("Class '%s' is not a UMaterialExpression subclass."), *ClassPath);
			return false;
		}

		OutClass = ExpressionClass;
		OutClassPath = ClassPath;
		return true;
	}

	static bool SetExpressionPropertyFromJson(
		UMaterialExpression* Expression,
		const FString& PropertyName,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutOldValue,
		FString& OutNewValue,
		FString& OutError)
	{
		if (!Expression)
		{
			OutError = TEXT("Expression is null.");
			return false;
		}
		if (PropertyName.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("property_name is required.");
			return false;
		}

		FProperty* Property = Expression->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Property '%s' was not found on %s."), *PropertyName, *Expression->GetClass()->GetName());
			return false;
		}

		Property->ExportText_Direct(
			OutOldValue,
			Property->ContainerPtrToValuePtr<void>(Expression),
			Property->ContainerPtrToValuePtr<void>(Expression),
			Expression,
			PPF_None);

		const FString ImportString = JsonValueToImportString(Value);
		Expression->Modify();
		const TCHAR* ImportResult = Property->ImportText_Direct(
			*ImportString,
			Property->ContainerPtrToValuePtr<void>(Expression),
			Expression,
			PPF_None);
		if (ImportResult == nullptr)
		{
			OutError = FString::Printf(TEXT("Could not import '%s' as %s."), *ImportString, *Property->GetCPPType());
			return false;
		}

		Property->ExportText_Direct(
			OutNewValue,
			Property->ContainerPtrToValuePtr<void>(Expression),
			Property->ContainerPtrToValuePtr<void>(Expression),
			Expression,
			PPF_None);
		return true;
	}

	static FString NodeFingerprint(const UMaterial* Material, const UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return TEXT("");
		}

		return FString::Printf(
			TEXT("%s|%s|%s|%d|%d"),
			*ExpressionId(Material, Expression),
			*Expression->GetClass()->GetPathName(),
			*Expression->GetParameterName().ToString(),
			Expression->MaterialExpressionEditorX,
			Expression->MaterialExpressionEditorY);
	}

	static TSharedRef<FJsonObject> NodeToJson(const UMaterial* Material, UMaterialExpression* Expression)
	{
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("id"), ExpressionId(Material, Expression));
		Node->SetNumberField(TEXT("index"), FindExpressionIndex(Material, Expression));
		Node->SetStringField(TEXT("guid"), Expression ? Expression->MaterialExpressionGuid.ToString() : TEXT(""));
		Node->SetStringField(TEXT("name"), Expression ? Expression->GetName() : TEXT(""));
		Node->SetStringField(TEXT("class"), Expression && Expression->GetClass() ? Expression->GetClass()->GetPathName() : TEXT(""));
		Node->SetStringField(TEXT("class_short"), ShortClassName(Expression));
		Node->SetStringField(TEXT("parameter_name"), Expression ? Expression->GetParameterName().ToString() : TEXT(""));
		Node->SetNumberField(TEXT("x"), Expression ? Expression->MaterialExpressionEditorX : 0);
		Node->SetNumberField(TEXT("y"), Expression ? Expression->MaterialExpressionEditorY : 0);
		Node->SetStringField(TEXT("fingerprint"), NodeFingerprint(Material, Expression));
		return Node;
	}

	static FString EdgeKey(const FString& Kind, const FString& FromId, const FString& FromOutput, const FString& ToId, const FString& ToInput)
	{
		return FString::Printf(TEXT("%s|%s|%s|%s|%s"), *Kind, *FromId, *FromOutput, *ToId, *ToInput);
	}

	static TSharedRef<FJsonObject> EdgeToJson(
		const FString& Kind,
		const FString& FromId,
		const FString& FromOutput,
		const FString& ToId,
		const FString& ToInput)
	{
		TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
		Edge->SetStringField(TEXT("key"), EdgeKey(Kind, FromId, FromOutput, ToId, ToInput));
		Edge->SetStringField(TEXT("kind"), Kind);
		Edge->SetStringField(TEXT("from_id"), FromId);
		Edge->SetStringField(TEXT("from_output"), FromOutput);
		if (Kind == TEXT("material_property"))
		{
			Edge->SetStringField(TEXT("to_property"), ToInput);
		}
		else
		{
			Edge->SetStringField(TEXT("to_id"), ToId);
			Edge->SetStringField(TEXT("to_input"), ToInput);
		}
		return Edge;
	}

	static void BuildGraphMaps(
		UMaterial* Material,
		TMap<FString, TSharedPtr<FJsonObject>>& OutNodes,
		TMap<FString, FString>& OutNodeFingerprints,
		TMap<FString, TSharedPtr<FJsonObject>>& OutEdges)
	{
		if (!Material)
		{
			return;
		}

		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (!Expression)
			{
				continue;
			}

			const FString Id = ExpressionId(Material, Expression);
			OutNodes.Add(Id, NodeToJson(Material, Expression));
			OutNodeFingerprints.Add(Id, NodeFingerprint(Material, Expression));

			for (FExpressionInputIterator InputIt{Expression}; InputIt; ++InputIt)
			{
				FExpressionInput* Input = InputIt.Input;
				if (!Input || !Input->Expression)
				{
					continue;
				}

				const FString FromId = ExpressionId(Material, Input->Expression);
				const FString ToInput = Expression->GetInputName(InputIt.Index).ToString();
				const FString FromOutput = OutputNameForIndex(Input->Expression, Input->OutputIndex);
				TSharedRef<FJsonObject> Edge = EdgeToJson(TEXT("expression_input"), FromId, FromOutput, Id, ToInput);
				OutEdges.Add(Edge->GetStringField(TEXT("key")), Edge);
			}
		}

		for (const TPair<FString, EMaterialProperty>& Pair : SupportedProperties())
		{
			FExpressionInput* Input = Material->GetExpressionInputForProperty(Pair.Value);
			if (!Input || !Input->Expression)
			{
				continue;
			}

			const FString FromId = ExpressionId(Material, Input->Expression);
			const FString FromOutput = OutputNameForIndex(Input->Expression, Input->OutputIndex);
			TSharedRef<FJsonObject> Edge = EdgeToJson(TEXT("material_property"), FromId, FromOutput, TEXT("material"), Pair.Key);
			OutEdges.Add(Edge->GetStringField(TEXT("key")), Edge);
		}
	}

	static TArray<TSharedPtr<FJsonValue>> SortedObjectValues(const TMap<FString, TSharedPtr<FJsonObject>>& Map)
	{
		TArray<FString> Keys;
		Map.GetKeys(Keys);
		Keys.Sort();

		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Key : Keys)
		{
			if (const TSharedPtr<FJsonObject>* Object = Map.Find(Key))
			{
				Values.Add(MakeShared<FJsonValueObject>(*Object));
			}
		}
		return Values;
	}

	static FString BuildMaterialGraphHash(
		const TMap<FString, FString>& NodeFingerprints,
		const TMap<FString, TSharedPtr<FJsonObject>>& Edges)
	{
		TArray<FString> NodeKeys;
		NodeFingerprints.GetKeys(NodeKeys);
		NodeKeys.Sort();

		TArray<FString> EdgeKeys;
		Edges.GetKeys(EdgeKeys);
		EdgeKeys.Sort();

		FString Basis = TEXT("material_graph_snapshot.v1|nodes:");
		for (const FString& Key : NodeKeys)
		{
			const FString* Fingerprint = NodeFingerprints.Find(Key);
			Basis += Key + TEXT("=") + (Fingerprint ? *Fingerprint : FString()) + TEXT(";");
		}
		Basis += TEXT("|edges:");
		for (const FString& Key : EdgeKeys)
		{
			Basis += Key + TEXT(";");
		}
		return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*Basis));
	}

	static TSharedRef<FJsonObject> BuildMaterialGraphSnapshot(UMaterial* Material, const FString& AssetPath, const FString& GraphPath)
	{
		TMap<FString, TSharedPtr<FJsonObject>> Nodes;
		TMap<FString, FString> Fingerprints;
		TMap<FString, TSharedPtr<FJsonObject>> Edges;
		BuildGraphMaps(Material, Nodes, Fingerprints, Edges);

		TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		Snapshot->SetStringField(TEXT("schema"), TEXT("somol.material_graph_snapshot.v1"));
		Snapshot->SetStringField(TEXT("asset_path"), AssetPath);
		Snapshot->SetStringField(TEXT("graph_asset_path"), GraphPath);
		Snapshot->SetStringField(TEXT("graph_hash"), BuildMaterialGraphHash(Fingerprints, Edges));
		Snapshot->SetNumberField(TEXT("node_count"), Nodes.Num());
		Snapshot->SetNumberField(TEXT("edge_count"), Edges.Num());
		Snapshot->SetArrayField(TEXT("nodes"), SortedObjectValues(Nodes));
		Snapshot->SetArrayField(TEXT("edges"), SortedObjectValues(Edges));
		return Snapshot;
	}

	static TSharedRef<FJsonObject> MaterialStatisticsToJson(const FMaterialStatistics& Stats)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("numVertexShaderInstructions"), Stats.NumVertexShaderInstructions);
		Json->SetNumberField(TEXT("numPixelShaderInstructions"), Stats.NumPixelShaderInstructions);
		Json->SetNumberField(TEXT("numSamplers"), Stats.NumSamplers);
		Json->SetNumberField(TEXT("numVertexTextureSamples"), Stats.NumVertexTextureSamples);
		Json->SetNumberField(TEXT("numPixelTextureSamples"), Stats.NumPixelTextureSamples);
		Json->SetNumberField(TEXT("numVirtualTextureSamples"), Stats.NumVirtualTextureSamples);
		Json->SetNumberField(TEXT("numUVScalars"), Stats.NumUVScalars);
		Json->SetNumberField(TEXT("numInterpolatorScalars"), Stats.NumInterpolatorScalars);
		return Json;
	}

	static bool RunMaterialMutationGate(
		const FString& AssetPath,
		const FString& GraphPath,
		UMaterial* Material,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		if (!Material)
		{
			OutError = TEXT("Material mutation gate missing material.");
			return false;
		}

		Material->PostEditChange();
		UMaterialEditingLibrary::RecompileMaterial(Material);
		const FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(Material);
		TSharedRef<FJsonObject> Snapshot = BuildMaterialGraphSnapshot(Material, AssetPath, GraphPath);

		const bool bStatsReadable = Stats.NumVertexShaderInstructions >= 0 &&
			Stats.NumPixelShaderInstructions >= 0 &&
			Stats.NumSamplers >= 0;
		TSharedRef<FJsonObject> Gate = MakeShared<FJsonObject>();
		Gate->SetStringField(TEXT("schema"), TEXT("somol.material_graph_mutation_gate.v1"));
		Gate->SetStringField(TEXT("asset_path"), AssetPath);
		Gate->SetStringField(TEXT("graph_asset_path"), GraphPath);
		Gate->SetStringField(TEXT("recompile_status"), TEXT("attempted"));
		Gate->SetStringField(TEXT("statistics_status"), bStatsReadable ? TEXT("read") : TEXT("failed"));
		Gate->SetObjectField(TEXT("statistics"), MaterialStatisticsToJson(Stats));
		Gate->SetObjectField(TEXT("post_compile_snapshot"), Snapshot);
		Gate->SetStringField(TEXT("receipt_status"), bStatsReadable ? TEXT("completed") : TEXT("failed_validation"));
		Gate->SetStringField(TEXT("required_before_delivery"), TEXT("material recompile + statistics + graph snapshot readback"));
		OutStructured->SetObjectField(TEXT("receipt_gate"), Gate);
		OutStructured->SetBoolField(TEXT("receipt_complete"), bStatsReadable);
		OutStructured->SetStringField(TEXT("receipt_status"), bStatsReadable ? TEXT("completed") : TEXT("failed_validation"));
		OutStructured->SetObjectField(TEXT("material_statistics"), MaterialStatisticsToJson(Stats));
		OutStructured->SetStringField(TEXT("post_compile_graph_hash"), Snapshot->GetStringField(TEXT("graph_hash")));

		if (!bStatsReadable)
		{
			OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("material_validation_failed"));
			OutError = TEXT("Material mutation failed receipt gate: recompile/statistics evidence was not readable.");
			return false;
		}
		return true;
	}

	static bool IsSafeDisposableMaterialPath(const FString& AssetPath)
	{
		return AssetPath.StartsWith(TEXT("/Game/SOMOLMCP/Disposable"));
	}

	static TMap<FString, TSharedPtr<FJsonObject>> JsonObjectMapFromArrayField(
		const TSharedPtr<FJsonObject>& Source,
		const TCHAR* ArrayField,
		const TCHAR* KeyField)
	{
		TMap<FString, TSharedPtr<FJsonObject>> Result;
		if (!Source.IsValid())
		{
			return Result;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Source->TryGetArrayField(ArrayField, Values) || !Values)
		{
			return Result;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Key;
			if (Object.IsValid() && Object->TryGetStringField(KeyField, Key) && !Key.TrimStartAndEnd().IsEmpty())
			{
				Result.Add(Key, Object);
			}
		}
		return Result;
	}

	static UMaterialExpression* FindExpressionBySnapshotId(UMaterial* Material, const FString& ExpressionIdValue)
	{
		if (!Material || ExpressionIdValue.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (Expression && ExpressionId(Material, Expression) == ExpressionIdValue)
			{
				return Expression;
			}
		}
		return nullptr;
	}

	static FExpressionInput* FindExpressionInputByName(UMaterialExpression* Expression, const FString& InputName)
	{
		if (!Expression || InputName.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		for (FExpressionInputIterator InputIt{Expression}; InputIt; ++InputIt)
		{
			if (Expression->GetInputName(InputIt.Index).ToString() == InputName)
			{
				return InputIt.Input;
			}
		}
		return nullptr;
	}

	static void AddStringReceipt(TArray<TSharedPtr<FJsonValue>>& Array, const FString& Field, const FString& Value)
	{
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(Field, Value);
		Array.Add(MakeShared<FJsonValueObject>(Receipt));
	}

	static bool RestoreSnapshotNodeFields(
		UMaterial* Material,
		UMaterialExpression* Expression,
		const TSharedPtr<FJsonObject>& SnapshotNode,
		TArray<TSharedPtr<FJsonValue>>& RestoredNodeFields,
		TArray<TSharedPtr<FJsonValue>>& IncompatibleNodes)
	{
		if (!Material || !Expression || !SnapshotNode.IsValid())
		{
			return false;
		}

		const FString Id = ExpressionId(Material, Expression);
		FString SnapshotClass;
		SnapshotNode->TryGetStringField(TEXT("class"), SnapshotClass);
		if (!SnapshotClass.IsEmpty() && Expression->GetClass()->GetPathName() != SnapshotClass)
		{
			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			Receipt->SetStringField(TEXT("id"), Id);
			Receipt->SetStringField(TEXT("current_class"), Expression->GetClass()->GetPathName());
			Receipt->SetStringField(TEXT("snapshot_class"), SnapshotClass);
			IncompatibleNodes.Add(MakeShared<FJsonValueObject>(Receipt));
			return false;
		}

		bool bChanged = false;
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("id"), Id);

		if (SnapshotNode->HasTypedField<EJson::Number>(TEXT("x")))
		{
			const int32 X = SnapshotNode->GetIntegerField(TEXT("x"));
			if (Expression->MaterialExpressionEditorX != X)
			{
				Expression->Modify();
				Expression->MaterialExpressionEditorX = X;
				Receipt->SetNumberField(TEXT("x"), X);
				bChanged = true;
			}
		}
		if (SnapshotNode->HasTypedField<EJson::Number>(TEXT("y")))
		{
			const int32 Y = SnapshotNode->GetIntegerField(TEXT("y"));
			if (Expression->MaterialExpressionEditorY != Y)
			{
				Expression->Modify();
				Expression->MaterialExpressionEditorY = Y;
				Receipt->SetNumberField(TEXT("y"), Y);
				bChanged = true;
			}
		}

		FString ParameterName;
		if (SnapshotNode->TryGetStringField(TEXT("parameter_name"), ParameterName))
		{
			if (UMaterialExpressionParameter* ParameterExpression = Cast<UMaterialExpressionParameter>(Expression))
			{
				if (ParameterExpression->ParameterName.ToString() != ParameterName)
				{
					ParameterExpression->Modify();
					ParameterExpression->ParameterName = FName(*ParameterName);
					Receipt->SetStringField(TEXT("parameter_name"), ParameterName);
					bChanged = true;
				}
			}
		}

		if (bChanged)
		{
			RestoredNodeFields.Add(MakeShared<FJsonValueObject>(Receipt));
		}
		return true;
	}

	static bool CreateExpressionFromSnapshotNode(
		FSololmcpEditorServices& Services,
		UMaterial* Material,
		const TSharedPtr<FJsonObject>& SnapshotNode,
		UMaterialExpression*& OutExpression,
		TSharedRef<FJsonObject>& OutReceipt,
		FString& OutError)
	{
		OutExpression = nullptr;
		if (!Material || !SnapshotNode.IsValid())
		{
			OutError = TEXT("Invalid material or snapshot node.");
			return false;
		}

		FString SnapshotClass;
		SnapshotNode->TryGetStringField(TEXT("class"), SnapshotClass);
		if (SnapshotClass.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("Snapshot node is missing class.");
			return false;
		}

		FString SnapshotId;
		SnapshotNode->TryGetStringField(TEXT("id"), SnapshotId);
		FString SnapshotGuid;
		SnapshotNode->TryGetStringField(TEXT("guid"), SnapshotGuid);
		const int32 X = SnapshotNode->HasTypedField<EJson::Number>(TEXT("x")) ? SnapshotNode->GetIntegerField(TEXT("x")) : 0;
		const int32 Y = SnapshotNode->HasTypedField<EJson::Number>(TEXT("y")) ? SnapshotNode->GetIntegerField(TEXT("y")) : 0;

		FString ResolveError;
		UClass* ExpressionClass = Services.ResolveClass(SnapshotClass, ResolveError);
		if (!ExpressionClass || !ExpressionClass->IsChildOf(UMaterialExpression::StaticClass()))
		{
			OutError = ResolveError.IsEmpty()
				? FString::Printf(TEXT("Snapshot node class is not a UMaterialExpression subclass: %s"), *SnapshotClass)
				: ResolveError;
			return false;
		}

		UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpression(Material, ExpressionClass, X, Y);
		if (!Expression)
		{
			OutError = FString::Printf(TEXT("CreateMaterialExpression failed while restoring snapshot node %s as %s."), *SnapshotId, *SnapshotClass);
			return false;
		}

		Expression->Modify();
		if (!SnapshotGuid.IsEmpty())
		{
			FGuid ParsedGuid;
			if (FGuid::Parse(SnapshotGuid, ParsedGuid))
			{
				Expression->MaterialExpressionGuid = ParsedGuid;
			}
			else
			{
				OutReceipt->SetStringField(TEXT("guid_restore_warning"), TEXT("snapshot_guid_parse_failed"));
			}
		}
		Expression->MaterialExpressionEditorX = X;
		Expression->MaterialExpressionEditorY = Y;

		FString ParameterName;
		if (SnapshotNode->TryGetStringField(TEXT("parameter_name"), ParameterName))
		{
			if (UMaterialExpressionParameter* ParameterExpression = Cast<UMaterialExpressionParameter>(Expression))
			{
				ParameterExpression->ParameterName = FName(*ParameterName);
			}
		}

		OutReceipt->SetStringField(TEXT("id"), SnapshotId);
		OutReceipt->SetStringField(TEXT("guid"), Expression->MaterialExpressionGuid.ToString());
		OutReceipt->SetStringField(TEXT("class"), Expression->GetClass()->GetPathName());
		OutReceipt->SetStringField(TEXT("class_short"), ShortClassName(Expression));
		OutReceipt->SetNumberField(TEXT("index"), FindExpressionIndex(Material, Expression));
		OutReceipt->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
		OutReceipt->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
		OutExpression = Expression;
		return true;
	}

	static bool DisconnectCurrentEdge(
		UMaterial* Material,
		const TSharedPtr<FJsonObject>& Edge,
		TArray<TSharedPtr<FJsonValue>>& DisconnectedEdges,
		FString& OutError)
	{
		if (!Material || !Edge.IsValid())
		{
			OutError = TEXT("Invalid edge object.");
			return false;
		}

		FString Kind;
		Edge->TryGetStringField(TEXT("kind"), Kind);
		if (Kind == TEXT("material_property"))
		{
			FString PropertyName;
			if (!Edge->TryGetStringField(TEXT("to_property"), PropertyName))
			{
				OutError = TEXT("material_property edge is missing to_property.");
				return false;
			}
			EMaterialProperty Property = MP_BaseColor;
			if (!TryParseMaterialProperty(PropertyName, Property, Material))
			{
				OutError = FString::Printf(TEXT("Unsupported material property '%s'."), *PropertyName);
				return false;
			}
			FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
			if (Input && Input->Expression)
			{
				Material->Modify();
				Input->Expression = nullptr;
				Input->OutputIndex = 0;
				TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
				Receipt->SetStringField(TEXT("kind"), Kind);
				Receipt->SetStringField(TEXT("to_property"), PropertyName);
				DisconnectedEdges.Add(MakeShared<FJsonValueObject>(Receipt));
			}
			return true;
		}

		if (Kind == TEXT("expression_input"))
		{
			FString ToId;
			FString ToInput;
			Edge->TryGetStringField(TEXT("to_id"), ToId);
			Edge->TryGetStringField(TEXT("to_input"), ToInput);
			UMaterialExpression* ToExpression = FindExpressionBySnapshotId(Material, ToId);
			FExpressionInput* Input = FindExpressionInputByName(ToExpression, ToInput);
			if (Input && Input->Expression)
			{
				ToExpression->Modify();
				Input->Expression = nullptr;
				Input->OutputIndex = 0;
				TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
				Receipt->SetStringField(TEXT("kind"), Kind);
				Receipt->SetStringField(TEXT("to_id"), ToId);
				Receipt->SetStringField(TEXT("to_input"), ToInput);
				DisconnectedEdges.Add(MakeShared<FJsonValueObject>(Receipt));
			}
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported snapshot edge kind '%s'."), *Kind);
		return false;
	}

	static bool ReconnectSnapshotEdge(
		UMaterial* Material,
		const TSharedPtr<FJsonObject>& Edge,
		TArray<TSharedPtr<FJsonValue>>& ReconnectedEdges,
		FString& OutError)
	{
		if (!Material || !Edge.IsValid())
		{
			OutError = TEXT("Invalid edge object.");
			return false;
		}

		FString Kind;
		FString FromId;
		FString FromOutput;
		Edge->TryGetStringField(TEXT("kind"), Kind);
		Edge->TryGetStringField(TEXT("from_id"), FromId);
		Edge->TryGetStringField(TEXT("from_output"), FromOutput);
		UMaterialExpression* FromExpression = FindExpressionBySnapshotId(Material, FromId);
		if (!FromExpression)
		{
			OutError = FString::Printf(TEXT("Snapshot edge source node is missing: %s"), *FromId);
			return false;
		}

		if (Kind == TEXT("material_property"))
		{
			FString PropertyName;
			Edge->TryGetStringField(TEXT("to_property"), PropertyName);
			EMaterialProperty Property = MP_BaseColor;
			if (!TryParseMaterialProperty(PropertyName, Property, Material))
			{
				OutError = FString::Printf(TEXT("Unsupported material property '%s'."), *PropertyName);
				return false;
			}
			if (!UMaterialEditingLibrary::ConnectMaterialProperty(FromExpression, FromOutput, Property))
			{
				OutError = FString::Printf(TEXT("Failed to reconnect material property '%s'."), *PropertyName);
				return false;
			}
			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			Receipt->SetStringField(TEXT("kind"), Kind);
			Receipt->SetStringField(TEXT("from_id"), FromId);
			Receipt->SetStringField(TEXT("from_output"), FromOutput);
			Receipt->SetStringField(TEXT("to_property"), PropertyName);
			ReconnectedEdges.Add(MakeShared<FJsonValueObject>(Receipt));
			return true;
		}

		if (Kind == TEXT("expression_input"))
		{
			FString ToId;
			FString ToInput;
			Edge->TryGetStringField(TEXT("to_id"), ToId);
			Edge->TryGetStringField(TEXT("to_input"), ToInput);
			UMaterialExpression* ToExpression = FindExpressionBySnapshotId(Material, ToId);
			if (!ToExpression)
			{
				OutError = FString::Printf(TEXT("Snapshot edge target node is missing: %s"), *ToId);
				return false;
			}
			if (!UMaterialEditingLibrary::ConnectMaterialExpressions(FromExpression, FromOutput, ToExpression, ToInput))
			{
				OutError = FString::Printf(TEXT("Failed to reconnect expression input '%s'."), *ToInput);
				return false;
			}
			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			Receipt->SetStringField(TEXT("kind"), Kind);
			Receipt->SetStringField(TEXT("from_id"), FromId);
			Receipt->SetStringField(TEXT("from_output"), FromOutput);
			Receipt->SetStringField(TEXT("to_id"), ToId);
			Receipt->SetStringField(TEXT("to_input"), ToInput);
			ReconnectedEdges.Add(MakeShared<FJsonValueObject>(Receipt));
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported snapshot edge kind '%s'."), *Kind);
		return false;
	}

	static bool RestoreMaterialGraphConservative(
		FSololmcpEditorServices& Services,
		UMaterial* Material,
		const FString& AssetPath,
		const FString& GraphPath,
		const TSharedPtr<FJsonObject>& Snapshot,
		const bool bSave,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		const TMap<FString, TSharedPtr<FJsonObject>> SnapshotNodes = JsonObjectMapFromArrayField(Snapshot, TEXT("nodes"), TEXT("id"));
		const TMap<FString, TSharedPtr<FJsonObject>> SnapshotEdges = JsonObjectMapFromArrayField(Snapshot, TEXT("edges"), TEXT("key"));
		if (SnapshotNodes.Num() == 0 && Snapshot->HasTypedField<EJson::Number>(TEXT("node_count")) && Snapshot->GetIntegerField(TEXT("node_count")) > 0)
		{
			OutStructured->SetStringField(TEXT("status"), TEXT("invalid_snapshot"));
			OutStructured->SetStringField(TEXT("error_code"), TEXT("SNAPSHOT_NODE_LIST_MISSING"));
			OutError = TEXT("Snapshot node list is missing.");
			return false;
		}

		TMap<FString, TSharedPtr<FJsonObject>> CurrentNodes;
		TMap<FString, FString> CurrentFingerprints;
		TMap<FString, TSharedPtr<FJsonObject>> CurrentEdges;
		BuildGraphMaps(Material, CurrentNodes, CurrentFingerprints, CurrentEdges);

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialGraphRestoreSnapshot", "SOMOLMCP Material Graph Restore Snapshot"));
		Material->Modify();

		TArray<TSharedPtr<FJsonValue>> RecreatedMissingNodes;
		TArray<TSharedPtr<FJsonValue>> ReplacedIncompatibleNodes;
		TArray<TSharedPtr<FJsonValue>> MissingNodes;
		TArray<TSharedPtr<FJsonValue>> IncompatibleNodes;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : SnapshotNodes)
		{
			UMaterialExpression* Expression = FindExpressionBySnapshotId(Material, Pair.Key);
			if (!Expression)
			{
				TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
				Receipt->SetStringField(TEXT("restore_action"), TEXT("recreate_missing_snapshot_node"));
				if (!CreateExpressionFromSnapshotNode(Services, Material, Pair.Value, Expression, Receipt, OutError))
				{
					Receipt->SetStringField(TEXT("id"), Pair.Key);
					Receipt->SetStringField(TEXT("error"), OutError);
					MissingNodes.Add(MakeShared<FJsonValueObject>(Receipt));
				}
				else
				{
					RecreatedMissingNodes.Add(MakeShared<FJsonValueObject>(Receipt));
				}
				continue;
			}
			FString SnapshotClass;
			Pair.Value->TryGetStringField(TEXT("class"), SnapshotClass);
			if (!SnapshotClass.IsEmpty() && Expression->GetClass()->GetPathName() != SnapshotClass)
			{
				TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
				Receipt->SetStringField(TEXT("id"), Pair.Key);
				Receipt->SetStringField(TEXT("current_class"), Expression->GetClass()->GetPathName());
				Receipt->SetStringField(TEXT("snapshot_class"), SnapshotClass);
				Receipt->SetStringField(TEXT("restore_action"), TEXT("replace_incompatible_snapshot_node_class"));
				UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
				UMaterialExpression* ReplacementExpression = nullptr;
				if (!CreateExpressionFromSnapshotNode(Services, Material, Pair.Value, ReplacementExpression, Receipt, OutError))
				{
					Receipt->SetStringField(TEXT("error"), OutError);
					IncompatibleNodes.Add(MakeShared<FJsonValueObject>(Receipt));
				}
				else
				{
					ReplacedIncompatibleNodes.Add(MakeShared<FJsonValueObject>(Receipt));
				}
			}
		}

		if (MissingNodes.Num() > 0 || IncompatibleNodes.Num() > 0)
		{
			OutStructured->SetStringField(TEXT("status"), TEXT("changed_graph_restore_blocked"));
			OutStructured->SetStringField(TEXT("error_code"), MissingNodes.Num() > 0 ? TEXT("MISSING_SNAPSHOT_NODES") : TEXT("INCOMPATIBLE_SNAPSHOT_NODES"));
			OutStructured->SetArrayField(TEXT("missing_nodes"), MissingNodes);
			OutStructured->SetArrayField(TEXT("incompatible_nodes"), IncompatibleNodes);
			OutStructured->SetArrayField(TEXT("recreated_missing_nodes"), RecreatedMissingNodes);
			OutStructured->SetArrayField(TEXT("replaced_incompatible_nodes"), ReplacedIncompatibleNodes);
			OutError = TEXT("Material graph restore could not recreate all missing nodes or replace all incompatible node classes from the snapshot.");
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> RestoredNodeFields;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : SnapshotNodes)
		{
			UMaterialExpression* Expression = FindExpressionBySnapshotId(Material, Pair.Key);
			if (Expression)
			{
				RestoreSnapshotNodeFields(Material, Expression, Pair.Value, RestoredNodeFields, IncompatibleNodes);
			}
		}

		TArray<UMaterialExpression*> ExtraExpressions;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (Expression && !SnapshotNodes.Contains(ExpressionId(Material, Expression)))
			{
				ExtraExpressions.Add(Expression);
			}
		}

		TArray<TSharedPtr<FJsonValue>> DeletedExtraNodes;
		for (UMaterialExpression* Expression : ExtraExpressions)
		{
			const FString Id = ExpressionId(Material, Expression);
			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			Receipt->SetStringField(TEXT("id"), Id);
			Receipt->SetStringField(TEXT("class"), Expression->GetClass()->GetPathName());
			Receipt->SetNumberField(TEXT("old_index"), FindExpressionIndex(Material, Expression));
			DeletedExtraNodes.Add(MakeShared<FJsonValueObject>(Receipt));
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
		}

		CurrentNodes.Reset();
		CurrentFingerprints.Reset();
		CurrentEdges.Reset();
		BuildGraphMaps(Material, CurrentNodes, CurrentFingerprints, CurrentEdges);

		TArray<TSharedPtr<FJsonValue>> DisconnectedExtraEdges;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : CurrentEdges)
		{
			if (!SnapshotEdges.Contains(Pair.Key))
			{
				if (!DisconnectCurrentEdge(Material, Pair.Value, DisconnectedExtraEdges, OutError))
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("changed_graph_restore_failed"));
					OutStructured->SetStringField(TEXT("error_code"), TEXT("DISCONNECT_EXTRA_EDGE_FAILED"));
					return false;
				}
			}
		}

		CurrentNodes.Reset();
		CurrentFingerprints.Reset();
		CurrentEdges.Reset();
		BuildGraphMaps(Material, CurrentNodes, CurrentFingerprints, CurrentEdges);

		TArray<TSharedPtr<FJsonValue>> ReconnectedMissingEdges;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : SnapshotEdges)
		{
			if (!CurrentEdges.Contains(Pair.Key))
			{
				if (!ReconnectSnapshotEdge(Material, Pair.Value, ReconnectedMissingEdges, OutError))
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("changed_graph_restore_failed"));
					OutStructured->SetStringField(TEXT("error_code"), TEXT("RECONNECT_SNAPSHOT_EDGE_FAILED"));
					return false;
				}
			}
		}

		Material->PostEditChange();
		Material->MarkPackageDirty();

		TSharedRef<FJsonObject> FinalSnapshot = BuildMaterialGraphSnapshot(Material, AssetPath, GraphPath);
		const FString FinalHash = FinalSnapshot->GetStringField(TEXT("graph_hash"));
		FString ExpectedHash;
		Snapshot->TryGetStringField(TEXT("graph_hash"), ExpectedHash);
		OutStructured->SetStringField(TEXT("post_hash"), FinalHash);
		OutStructured->SetObjectField(TEXT("post_snapshot"), FinalSnapshot);
		OutStructured->SetArrayField(TEXT("restored_node_fields"), RestoredNodeFields);
		OutStructured->SetArrayField(TEXT("recreated_missing_nodes"), RecreatedMissingNodes);
		OutStructured->SetArrayField(TEXT("replaced_incompatible_nodes"), ReplacedIncompatibleNodes);
		OutStructured->SetArrayField(TEXT("deleted_extra_nodes"), DeletedExtraNodes);
		OutStructured->SetArrayField(TEXT("disconnected_extra_edges"), DisconnectedExtraEdges);
		OutStructured->SetArrayField(TEXT("reconnected_missing_edges"), ReconnectedMissingEdges);

		if (FinalHash != ExpectedHash)
		{
			OutStructured->SetStringField(TEXT("status"), TEXT("changed_graph_restore_incomplete"));
			OutStructured->SetStringField(TEXT("error_code"), TEXT("POST_RESTORE_HASH_MISMATCH"));
			OutError = FString::Printf(TEXT("Conservative restore completed but hash mismatch remains: expected %s got %s."), *ExpectedHash, *FinalHash);
			return false;
		}

		bool bSaved = false;
		FString SaveError;
		if (bSave)
		{
			bSaved = Services.SaveAsset(AssetPath, false, SaveError);
			OutStructured->SetBoolField(TEXT("saved"), bSaved);
			if (!bSaved)
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("restored_save_failed"));
				OutStructured->SetStringField(TEXT("error_code"), TEXT("SAVE_FAILED"));
				OutError = SaveError.IsEmpty() ? TEXT("Material graph restored but SaveAsset failed.") : SaveError;
				return false;
			}
		}
		else
		{
			OutStructured->SetBoolField(TEXT("saved"), false);
		}

		OutStructured->SetStringField(TEXT("status"), TEXT("restored"));
		OutStructured->SetStringField(TEXT("restore_mode"), RecreatedMissingNodes.Num() > 0 || ReplacedIncompatibleNodes.Num() > 0
			? TEXT("conservative_node_recreate_class_replace_edge_restore")
			: TEXT("conservative_extra_node_edge_restore"));
		return true;
	}

	static TSharedRef<FJsonObject> MakePatchOperationSchemaJson()
	{
		TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("contract_version"), ContractVersion);
		Schema->SetStringField(TEXT("operation_id"), TEXT("required stable client id per operation"));
		Schema->SetStringField(
			TEXT("supported_ops"),
			TEXT("set_expression_property | connect_property | connect_expression | delete_expression | create_expression | promote_parameter"));
		Schema->SetStringField(TEXT("set_expression_property"), TEXT("{op, operation_id, expression_guid|expression_index, property_name, value}"));
		Schema->SetStringField(TEXT("connect_property"), TEXT("{op, operation_id, from_expression_guid|from_expression_index, property, output_name?}"));
		Schema->SetStringField(TEXT("connect_expression"), TEXT("{op, operation_id, from_expression_guid|from_expression_index, to_expression_guid|to_expression_index, to_input, output_name?}"));
		Schema->SetStringField(TEXT("delete_expression"), TEXT("{op, operation_id, expression_guid|expression_index}"));
		Schema->SetStringField(TEXT("create_expression"), TEXT("{op, operation_id, expression_type|class, name?, x?, y?, properties?}"));
		Schema->SetStringField(TEXT("promote_parameter"), TEXT("{op, operation_id, expression_guid|expression_index, parameter_name, parameter_type?}"));
		return Schema;
	}

	static bool HasAnyField(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		for (const FString& FieldName : FieldNames)
		{
			if (Object->HasField(FieldName))
			{
				return true;
			}
		}
		return false;
	}

	static TSharedRef<FJsonObject> ValidatePatchOperation(const TSharedPtr<FJsonObject>& Operation, const int32 Index)
	{
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetNumberField(TEXT("index"), Index);

		if (!Operation.IsValid())
		{
			Receipt->SetBoolField(TEXT("valid"), false);
			Receipt->SetStringField(TEXT("error"), TEXT("Operation must be an object."));
			return Receipt;
		}

		FString Op;
		Operation->TryGetStringField(TEXT("op"), Op);
		Receipt->SetStringField(TEXT("op"), Op);

		FString OperationId;
		Operation->TryGetStringField(TEXT("operation_id"), OperationId);
		Receipt->SetStringField(TEXT("operation_id"), OperationId);

		TArray<FString> Errors;
		if (OperationId.TrimStartAndEnd().IsEmpty())
		{
			Errors.Add(TEXT("operation_id is required"));
		}

		if (Op == TEXT("set_expression_property"))
		{
			if (!HasAnyField(Operation, {TEXT("expression_guid"), TEXT("expression_index")}))
			{
				Errors.Add(TEXT("expression_guid or expression_index is required"));
			}
			if (!Operation->HasField(TEXT("property_name")))
			{
				Errors.Add(TEXT("property_name is required"));
			}
			if (!Operation->HasField(TEXT("value")))
			{
				Errors.Add(TEXT("value is required"));
			}
		}
		else if (Op == TEXT("connect_property"))
		{
			if (!HasAnyField(Operation, {TEXT("from_expression_guid"), TEXT("from_expression_index")}))
			{
				Errors.Add(TEXT("from_expression_guid or from_expression_index is required"));
			}
			if (!Operation->HasField(TEXT("property")))
			{
				Errors.Add(TEXT("property is required"));
			}
		}
		else if (Op == TEXT("connect_expression"))
		{
			if (!HasAnyField(Operation, {TEXT("from_expression_guid"), TEXT("from_expression_index")}))
			{
				Errors.Add(TEXT("from_expression_guid or from_expression_index is required"));
			}
			if (!HasAnyField(Operation, {TEXT("to_expression_guid"), TEXT("to_expression_index")}))
			{
				Errors.Add(TEXT("to_expression_guid or to_expression_index is required"));
			}
			if (!Operation->HasField(TEXT("to_input")))
			{
				Errors.Add(TEXT("to_input is required"));
			}
		}
		else if (Op == TEXT("delete_expression"))
		{
			if (!HasAnyField(Operation, {TEXT("expression_guid"), TEXT("expression_index")}))
			{
				Errors.Add(TEXT("expression_guid or expression_index is required"));
			}
		}
		else if (Op == TEXT("create_expression"))
		{
			if (!HasAnyField(Operation, {TEXT("expression_type"), TEXT("class")}))
			{
				Errors.Add(TEXT("expression_type or class is required"));
			}
		}
		else if (Op == TEXT("promote_parameter"))
		{
			if (!HasAnyField(Operation, {TEXT("expression_guid"), TEXT("expression_index")}))
			{
				Errors.Add(TEXT("expression_guid or expression_index is required"));
			}
			if (!Operation->HasField(TEXT("parameter_name")))
			{
				Errors.Add(TEXT("parameter_name is required"));
			}
		}
		else
		{
			Errors.Add(TEXT("unsupported op"));
		}

		TArray<TSharedPtr<FJsonValue>> ErrorJson;
		for (const FString& Error : Errors)
		{
			ErrorJson.Add(MakeShared<FJsonValueString>(Error));
		}
		Receipt->SetArrayField(TEXT("errors"), ErrorJson);
		Receipt->SetBoolField(TEXT("valid"), Errors.IsEmpty());
		return Receipt;
	}

	static TSharedRef<FJsonObject> PatchOperationInputSchema()
	{
		using SB = FSololmcpSchemaBuilder;
		return SB::Object({
			{TEXT("op"), SB::String(
				TEXT("Patch operation type."),
				{TEXT("set_expression_property"), TEXT("connect_property"), TEXT("connect_expression"), TEXT("delete_expression"), TEXT("create_expression"), TEXT("promote_parameter")})},
			{TEXT("operation_id"), SB::String(TEXT("Stable client-provided id for receipts and retries."))},
			{TEXT("expression_guid"), SB::String(TEXT("Target expression GUID for set/delete/promote."))},
			{TEXT("expression_index"), SB::Integer(TEXT("Fallback target expression index for set/delete/promote."))},
			{TEXT("from_expression_guid"), SB::String(TEXT("Source expression GUID for connect operations."))},
			{TEXT("from_expression_index"), SB::Integer(TEXT("Fallback source expression index for connect operations."))},
			{TEXT("to_expression_guid"), SB::String(TEXT("Destination expression GUID for connect_expression."))},
			{TEXT("to_expression_index"), SB::Integer(TEXT("Fallback destination expression index for connect_expression."))},
			{TEXT("expression_type"), SB::String(TEXT("MaterialExpression short type for create_expression, e.g. TextureSample, Multiply."))},
			{TEXT("class"), SB::String(TEXT("Full UClass path/name for create_expression."))},
			{TEXT("name"), SB::String(TEXT("Optional expression display/name hint."))},
			{TEXT("property_name"), SB::String(TEXT("UObject editor property for set_expression_property."))},
			{TEXT("property"), SB::String(TEXT("Material property for connect_property, e.g. BaseColor, Roughness."))},
			{TEXT("to_input"), SB::String(TEXT("Destination expression input name for connect_expression."))},
			{TEXT("output_name"), SB::String(TEXT("Optional source output name."))},
			{TEXT("parameter_name"), SB::String(TEXT("New parameter name for promote_parameter."))},
			{TEXT("parameter_type"), SB::String(TEXT("Optional parameter type hint for promote_parameter."))},
			{TEXT("x"), SB::Integer(TEXT("Editor X position for create_expression."))},
			{TEXT("y"), SB::Integer(TEXT("Editor Y position for create_expression."))},
			{TEXT("value"), SB::Object({}, {}, TEXT("JSON value for set_expression_property. Scalars may also be accepted by the runtime parser."))},
			{TEXT("properties"), SB::Object({}, {}, TEXT("Initial property bag for create_expression."))}
		});
	}

	static UMaterialExpression* ResolveExpressionTarget(
		UMaterial* Material,
		const TSharedRef<FJsonObject>& Arguments,
		FString& OutTargetId,
		FString& OutError)
	{
		if (!Material)
		{
			OutError = TEXT("Material graph is null.");
			return nullptr;
		}

		FString ExpressionGuid;
		if (Arguments->TryGetStringField(TEXT("expression_guid"), ExpressionGuid) &&
			!ExpressionGuid.TrimStartAndEnd().IsEmpty())
		{
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (Expression && Expression->MaterialExpressionGuid.ToString() == ExpressionGuid)
				{
					OutTargetId = ExpressionId(Material, Expression);
					return Expression;
				}
			}
			OutError = FString::Printf(TEXT("No material expression matched expression_guid %s."), *ExpressionGuid);
			return nullptr;
		}

		double ExpressionIndexValue = 0.0;
		if (Arguments->TryGetNumberField(TEXT("expression_index"), ExpressionIndexValue))
		{
			const int32 ExpressionIndex = FMath::RoundToInt(ExpressionIndexValue);
			const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();
			if (Expressions.IsValidIndex(ExpressionIndex) && Expressions[ExpressionIndex])
			{
				OutTargetId = ExpressionId(Material, Expressions[ExpressionIndex]);
				return Expressions[ExpressionIndex];
			}
			OutError = FString::Printf(TEXT("expression_index %d is out of range."), ExpressionIndex);
			return nullptr;
		}

		OutError = TEXT("expression_guid or expression_index is required.");
		return nullptr;
	}

	static UMaterialExpression* ResolveExpressionTargetWithPrefix(
		UMaterial* Material,
		const TSharedRef<FJsonObject>& Operation,
		const TCHAR* GuidField,
		const TCHAR* IndexField,
		FString& OutTargetId,
		FString& OutError)
	{
		TSharedRef<FJsonObject> Normalized = MakeShared<FJsonObject>();
		FString Guid;
		if (Operation->TryGetStringField(GuidField, Guid))
		{
			Normalized->SetStringField(TEXT("expression_guid"), Guid);
		}
		double IndexValue = 0.0;
		if (Operation->TryGetNumberField(IndexField, IndexValue))
		{
			Normalized->SetNumberField(TEXT("expression_index"), IndexValue);
		}
		return ResolveExpressionTarget(Material, Normalized, OutTargetId, OutError);
	}

	static bool ApplySetExpressionProperty(
		UMaterial* Material,
		const TSharedRef<FJsonObject>& Operation,
		TSharedRef<FJsonObject>& Receipt,
		FString& OutError)
	{
		FString TargetId;
		UMaterialExpression* Expression = ResolveExpressionTarget(Material, Operation, TargetId, OutError);
		if (!Expression)
		{
			return false;
		}

		FString PropertyName;
		if (!Operation->TryGetStringField(TEXT("property_name"), PropertyName))
		{
			OutError = TEXT("property_name is required.");
			return false;
		}
		FString OldValue;
		FString NewValue;
		if (!SetExpressionPropertyFromJson(Expression, PropertyName, Operation->TryGetField(TEXT("value")), OldValue, NewValue, OutError))
		{
			return false;
		}
		Receipt->SetStringField(TEXT("target_expression_id"), TargetId);
		Receipt->SetStringField(TEXT("property_name"), PropertyName);
		Receipt->SetStringField(TEXT("old_value"), OldValue);
		Receipt->SetStringField(TEXT("new_value"), NewValue);
		return true;
	}

	static bool ApplyCreateExpression(
		FSololmcpEditorServices& Services,
		UMaterial* Material,
		const TSharedRef<FJsonObject>& Operation,
		TSharedRef<FJsonObject>& Receipt,
		FString& OutError)
	{
		UClass* ExpressionClass = nullptr;
		FString ClassPath;
		if (!ResolveMaterialExpressionClass(Services, Operation, ExpressionClass, ClassPath, OutError))
		{
			return false;
		}

		const int32 NodeX = Operation->HasTypedField<EJson::Number>(TEXT("x")) ? Operation->GetIntegerField(TEXT("x")) : 0;
		const int32 NodeY = Operation->HasTypedField<EJson::Number>(TEXT("y")) ? Operation->GetIntegerField(TEXT("y")) : 0;
		UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpression(Material, ExpressionClass, NodeX, NodeY);
		if (!Expression)
		{
			OutError = FString::Printf(TEXT("CreateMaterialExpression failed for '%s'."), *ClassPath);
			return false;
		}
		if (FindExpressionIndex(Material, Expression) == INDEX_NONE || !Expression->IsA(ExpressionClass))
		{
			OutError = TEXT("CreateMaterialExpression returned an expression that did not read back from the material graph.");
			return false;
		}

		FString Name;
		if (Operation->TryGetStringField(TEXT("name"), Name) && !Name.TrimStartAndEnd().IsEmpty())
		{
			if (UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
			{
				Parameter->Modify();
				Parameter->ParameterName = FName(*Name);
			}
		}

		TArray<TSharedPtr<FJsonValue>> PropertyReceipts;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (Operation->TryGetObjectField(TEXT("properties"), Properties) && Properties && Properties->IsValid())
		{
			for (const auto& Pair : (*Properties)->Values)
			{
				const FString Key(*Pair.Key);
				FString OldValue;
				FString NewValue;
				FString PropertyError;
				TSharedRef<FJsonObject> PropertyReceipt = MakeShared<FJsonObject>();
				PropertyReceipt->SetStringField(TEXT("property_name"), Key);
				const bool bSet = SetExpressionPropertyFromJson(Expression, Key, Pair.Value, OldValue, NewValue, PropertyError);
				PropertyReceipt->SetBoolField(TEXT("applied"), bSet);
				if (bSet)
				{
					PropertyReceipt->SetStringField(TEXT("old_value"), OldValue);
					PropertyReceipt->SetStringField(TEXT("new_value"), NewValue);
				}
				else
				{
					PropertyReceipt->SetStringField(TEXT("error"), PropertyError);
					OutError = FString::Printf(TEXT("create_expression property '%s' failed: %s"), *Pair.Key, *PropertyError);
					PropertyReceipts.Add(MakeShared<FJsonValueObject>(PropertyReceipt));
					Receipt->SetArrayField(TEXT("property_receipts"), PropertyReceipts);
					return false;
				}
				PropertyReceipts.Add(MakeShared<FJsonValueObject>(PropertyReceipt));
			}
		}

		Receipt->SetStringField(TEXT("created_expression_id"), ExpressionId(Material, Expression));
		Receipt->SetNumberField(TEXT("created_expression_index"), FindExpressionIndex(Material, Expression));
		Receipt->SetStringField(TEXT("created_expression_class"), Expression->GetClass()->GetPathName());
		Receipt->SetStringField(TEXT("created_expression_class_short"), ShortClassName(Expression));
		Receipt->SetStringField(TEXT("created_expression_guid"), Expression->MaterialExpressionGuid.ToString());
		Receipt->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
		Receipt->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
		Receipt->SetArrayField(TEXT("property_receipts"), PropertyReceipts);
		return true;
	}

	struct FCapturedExpressionLink
	{
		bool bMaterialProperty = false;
		EMaterialProperty MaterialProperty = MP_BaseColor;
		FString MaterialPropertyName;
		UMaterialExpression* ToExpression = nullptr;
		FString ToInput;
		FString OutputName;
	};

	static TArray<FCapturedExpressionLink> CaptureOutboundLinks(UMaterial* Material, UMaterialExpression* SourceExpression)
	{
		TArray<FCapturedExpressionLink> Links;
		if (!Material || !SourceExpression)
		{
			return Links;
		}

		for (const TPair<FString, EMaterialProperty>& Pair : SupportedProperties())
		{
			FExpressionInput* Input = Material->GetExpressionInputForProperty(Pair.Value);
			if (Input && Input->Expression == SourceExpression)
			{
				FCapturedExpressionLink Link;
				Link.bMaterialProperty = true;
				Link.MaterialProperty = Pair.Value;
				Link.MaterialPropertyName = Pair.Key;
				Link.OutputName = OutputNameForIndex(SourceExpression, Input->OutputIndex);
				Links.Add(Link);
			}
		}

		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (!Expression || Expression == SourceExpression)
			{
				continue;
			}
			for (FExpressionInputIterator InputIt{Expression}; InputIt; ++InputIt)
			{
				FExpressionInput* Input = InputIt.Input;
				if (Input && Input->Expression == SourceExpression)
				{
					FCapturedExpressionLink Link;
					Link.ToExpression = Expression;
					Link.ToInput = Expression->GetInputName(InputIt.Index).ToString();
					Link.OutputName = OutputNameForIndex(SourceExpression, Input->OutputIndex);
					Links.Add(Link);
				}
			}
		}
		return Links;
	}

	static bool ReconnectOutboundLinks(
		UMaterial* Material,
		UMaterialExpression* NewExpression,
		const TArray<FCapturedExpressionLink>& Links,
		TSharedRef<FJsonObject>& Receipt,
		FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> Reconnected;
		for (const FCapturedExpressionLink& Link : Links)
		{
			TSharedRef<FJsonObject> LinkReceipt = MakeShared<FJsonObject>();
			LinkReceipt->SetStringField(TEXT("output_name"), Link.OutputName);
			if (Link.bMaterialProperty)
			{
				if (!UMaterialEditingLibrary::ConnectMaterialProperty(NewExpression, Link.OutputName, Link.MaterialProperty))
				{
					OutError = FString::Printf(TEXT("Failed to reconnect promoted parameter to material property '%s'."), *Link.MaterialPropertyName);
					return false;
				}
				LinkReceipt->SetStringField(TEXT("kind"), TEXT("material_property"));
				LinkReceipt->SetStringField(TEXT("property"), Link.MaterialPropertyName);
			}
			else
			{
				if (!Link.ToExpression || !UMaterialEditingLibrary::ConnectMaterialExpressions(NewExpression, Link.OutputName, Link.ToExpression, Link.ToInput))
				{
					OutError = FString::Printf(TEXT("Failed to reconnect promoted parameter to expression input '%s'."), *Link.ToInput);
					return false;
				}
				LinkReceipt->SetStringField(TEXT("kind"), TEXT("expression_input"));
				LinkReceipt->SetStringField(TEXT("to_expression_id"), ExpressionId(Material, Link.ToExpression));
				LinkReceipt->SetStringField(TEXT("to_input"), Link.ToInput);
			}
			Reconnected.Add(MakeShared<FJsonValueObject>(LinkReceipt));
		}
		Receipt->SetArrayField(TEXT("reconnected_links"), Reconnected);
		return true;
	}

	static bool ApplyPromoteParameter(
		UMaterial* Material,
		const TSharedRef<FJsonObject>& Operation,
		TSharedRef<FJsonObject>& Receipt,
		FString& OutError)
	{
		FString TargetId;
		UMaterialExpression* Expression = ResolveExpressionTarget(Material, Operation, TargetId, OutError);
		if (!Expression)
		{
			return false;
		}

		FString ParameterName;
		if (!Operation->TryGetStringField(TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("parameter_name is required.");
			return false;
		}

		const FString OldClass = Expression->GetClass()->GetPathName();
		const int32 OldIndex = FindExpressionIndex(Material, Expression);
		if (UMaterialExpressionParameter* ExistingParameter = Cast<UMaterialExpressionParameter>(Expression))
		{
			const FString OldName = ExistingParameter->ParameterName.ToString();
			ExistingParameter->Modify();
			ExistingParameter->ParameterName = FName(*ParameterName);
			Receipt->SetStringField(TEXT("target_expression_id"), TargetId);
			Receipt->SetStringField(TEXT("promotion_mode"), TEXT("rename_existing_parameter"));
			Receipt->SetStringField(TEXT("old_class"), OldClass);
			Receipt->SetStringField(TEXT("old_parameter_name"), OldName);
			Receipt->SetStringField(TEXT("new_parameter_name"), ParameterName);
			Receipt->SetNumberField(TEXT("expression_index"), OldIndex);
			return true;
		}

		UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expression);
		if (!Constant)
		{
			OutError = FString::Printf(TEXT("promote_parameter supports existing parameter expressions and Constant -> ScalarParameter in this hardened subset; got %s."), *ShortClassName(Expression));
			return false;
		}

		const float DefaultValue = Constant->R;
		const TArray<FCapturedExpressionLink> Links = CaptureOutboundLinks(Material, Expression);
		UMaterialExpressionScalarParameter* NewParameter = Cast<UMaterialExpressionScalarParameter>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				Material,
				UMaterialExpressionScalarParameter::StaticClass(),
				Expression->MaterialExpressionEditorX,
				Expression->MaterialExpressionEditorY));
		if (!NewParameter)
		{
			OutError = TEXT("Failed to create ScalarParameter for Constant promotion.");
			return false;
		}
		NewParameter->Modify();
		NewParameter->ParameterName = FName(*ParameterName);
		NewParameter->DefaultValue = DefaultValue;
		if (!ReconnectOutboundLinks(Material, NewParameter, Links, Receipt, OutError))
		{
			return false;
		}
		UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);

		Receipt->SetStringField(TEXT("target_expression_id"), TargetId);
		Receipt->SetStringField(TEXT("promotion_mode"), TEXT("constant_to_scalar_parameter"));
		Receipt->SetStringField(TEXT("old_class"), OldClass);
		Receipt->SetNumberField(TEXT("old_expression_index"), OldIndex);
		Receipt->SetStringField(TEXT("new_expression_id"), ExpressionId(Material, NewParameter));
		Receipt->SetNumberField(TEXT("new_expression_index"), FindExpressionIndex(Material, NewParameter));
		Receipt->SetStringField(TEXT("new_expression_guid"), NewParameter->MaterialExpressionGuid.ToString());
		Receipt->SetStringField(TEXT("new_parameter_name"), ParameterName);
		Receipt->SetNumberField(TEXT("default_value"), DefaultValue);
		return true;
	}

	static bool ApplyConnectProperty(
		UMaterial* Material,
		const TSharedRef<FJsonObject>& Operation,
		TSharedRef<FJsonObject>& Receipt,
		FString& OutError)
	{
		FString FromId;
		UMaterialExpression* FromExpression = ResolveExpressionTargetWithPrefix(
			Material,
			Operation,
			TEXT("from_expression_guid"),
			TEXT("from_expression_index"),
			FromId,
			OutError);
		if (!FromExpression)
		{
			return false;
		}

		FString PropertyName;
		if (!Operation->TryGetStringField(TEXT("property"), PropertyName))
		{
			OutError = TEXT("property is required.");
			return false;
		}
		EMaterialProperty Property = MP_BaseColor;
		if (!TryParseMaterialProperty(PropertyName, Property, Material))
		{
			OutError = FString::Printf(TEXT("Unsupported material property '%s'."), *PropertyName);
			return false;
		}

		FString OutputName;
		Operation->TryGetStringField(TEXT("output_name"), OutputName);
		if (!UMaterialEditingLibrary::ConnectMaterialProperty(FromExpression, OutputName, Property))
		{
			OutError = FString::Printf(TEXT("ConnectMaterialProperty failed for '%s'."), *PropertyName);
			return false;
		}

		FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
		if (!Input || Input->Expression != FromExpression)
		{
			OutError = TEXT("ConnectMaterialProperty returned success but readback did not match.");
			return false;
		}
		Receipt->SetStringField(TEXT("from_expression_id"), FromId);
		Receipt->SetStringField(TEXT("property"), PropertyName);
		Receipt->SetStringField(TEXT("output_name"), OutputName);
		return true;
	}

	static bool ApplyConnectExpression(
		UMaterial* Material,
		const TSharedRef<FJsonObject>& Operation,
		TSharedRef<FJsonObject>& Receipt,
		FString& OutError)
	{
		FString FromId;
		FString ToId;
		UMaterialExpression* FromExpression = ResolveExpressionTargetWithPrefix(
			Material, Operation, TEXT("from_expression_guid"), TEXT("from_expression_index"), FromId, OutError);
		if (!FromExpression)
		{
			return false;
		}
		UMaterialExpression* ToExpression = ResolveExpressionTargetWithPrefix(
			Material, Operation, TEXT("to_expression_guid"), TEXT("to_expression_index"), ToId, OutError);
		if (!ToExpression)
		{
			return false;
		}

		FString OutputName;
		FString ToInput;
		Operation->TryGetStringField(TEXT("output_name"), OutputName);
		if (!Operation->TryGetStringField(TEXT("to_input"), ToInput) || ToInput.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("to_input is required.");
			return false;
		}
		if (!UMaterialEditingLibrary::ConnectMaterialExpressions(FromExpression, OutputName, ToExpression, ToInput))
		{
			OutError = TEXT("ConnectMaterialExpressions failed.");
			return false;
		}
		if (!UMaterialEditingLibrary::GetInputsForMaterialExpression(Material, ToExpression).Contains(FromExpression))
		{
			OutError = TEXT("ConnectMaterialExpressions returned success but readback did not match.");
			return false;
		}
		Receipt->SetStringField(TEXT("from_expression_id"), FromId);
		Receipt->SetStringField(TEXT("to_expression_id"), ToId);
		Receipt->SetStringField(TEXT("to_input"), ToInput);
		Receipt->SetStringField(TEXT("output_name"), OutputName);
		return true;
	}

	static bool ApplyDeleteExpression(
		UMaterial* Material,
		const TSharedRef<FJsonObject>& Operation,
		TSharedRef<FJsonObject>& Receipt,
		FString& OutError)
	{
		FString TargetId;
		UMaterialExpression* Expression = ResolveExpressionTarget(Material, Operation, TargetId, OutError);
		if (!Expression)
		{
			return false;
		}
		const int32 OldIndex = FindExpressionIndex(Material, Expression);
		UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
		if (FindExpressionIndex(Material, Expression) != INDEX_NONE)
		{
			OutError = TEXT("DeleteMaterialExpression returned but expression is still present.");
			return false;
		}
		Receipt->SetStringField(TEXT("target_expression_id"), TargetId);
		Receipt->SetNumberField(TEXT("old_expression_index"), OldIndex);
		return true;
	}

	static bool ApplyPatchOperation(
		FSololmcpEditorServices& Services,
		UMaterial* Material,
		const TSharedPtr<FJsonObject>& Operation,
		TSharedRef<FJsonObject>& Receipt,
		FString& OutError)
	{
		if (!Operation.IsValid())
		{
			OutError = TEXT("Operation must be an object.");
			return false;
		}
		FString Op;
		Operation->TryGetStringField(TEXT("op"), Op);
		if (Op == TEXT("set_expression_property"))
		{
			return ApplySetExpressionProperty(Material, Operation.ToSharedRef(), Receipt, OutError);
		}
		if (Op == TEXT("connect_property"))
		{
			return ApplyConnectProperty(Material, Operation.ToSharedRef(), Receipt, OutError);
		}
		if (Op == TEXT("connect_expression"))
		{
			return ApplyConnectExpression(Material, Operation.ToSharedRef(), Receipt, OutError);
		}
		if (Op == TEXT("delete_expression"))
		{
			return ApplyDeleteExpression(Material, Operation.ToSharedRef(), Receipt, OutError);
		}
		if (Op == TEXT("create_expression"))
		{
			return ApplyCreateExpression(Services, Material, Operation.ToSharedRef(), Receipt, OutError);
		}
		if (Op == TEXT("promote_parameter"))
		{
			return ApplyPromoteParameter(Material, Operation.ToSharedRef(), Receipt, OutError);
		}
		OutError = FString::Printf(TEXT("Unsupported patch op '%s'."), *Op);
		return false;
	}
}

void RegisterMaterialGraphPatchTools(FSololmcpToolRegistry& Registry)
{
	using namespace MaterialGraphPatchTools;
	using SB = FSololmcpSchemaBuilder;

	Registry.Register({
		TEXT("material_graph_diff"),
		TEXT("Diff two material graphs by expression nodes and connections. Read-only; accepts UMaterial or MaterialInstance assets."),
		SB::Object({
			{TEXT("asset_path_a"), SB::String(TEXT("Baseline material asset path."))},
			{TEXT("asset_path_b"), SB::String(TEXT("Candidate material asset path."))},
			{TEXT("include_unchanged"), SB::Boolean(TEXT("Include unchanged node/edge counts; default true."))}
		}, {TEXT("asset_path_a"), TEXT("asset_path_b")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPathA;
			FString AssetPathB;
			if (!Arguments->TryGetStringField(TEXT("asset_path_a"), AssetPathA) ||
				!Arguments->TryGetStringField(TEXT("asset_path_b"), AssetPathB))
			{
				OutError = TEXT("Missing asset_path_a or asset_path_b.");
				return false;
			}

			UMaterial* MaterialA = nullptr;
			UMaterial* MaterialB = nullptr;
			FString GraphPathA;
			FString GraphPathB;
			if (!LoadMaterialForGraph(Context.Services, AssetPathA, MaterialA, GraphPathA, OutError) ||
				!LoadMaterialForGraph(Context.Services, AssetPathB, MaterialB, GraphPathB, OutError))
			{
				return false;
			}

			TMap<FString, TSharedPtr<FJsonObject>> NodesA;
			TMap<FString, TSharedPtr<FJsonObject>> NodesB;
			TMap<FString, FString> FingerprintsA;
			TMap<FString, FString> FingerprintsB;
			TMap<FString, TSharedPtr<FJsonObject>> EdgesA;
			TMap<FString, TSharedPtr<FJsonObject>> EdgesB;
			BuildGraphMaps(MaterialA, NodesA, FingerprintsA, EdgesA);
			BuildGraphMaps(MaterialB, NodesB, FingerprintsB, EdgesB);

			TMap<FString, TSharedPtr<FJsonObject>> AddedNodes;
			TMap<FString, TSharedPtr<FJsonObject>> RemovedNodes;
			TArray<TSharedPtr<FJsonValue>> ChangedNodes;
			for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : NodesB)
			{
				if (!NodesA.Contains(Pair.Key))
				{
					AddedNodes.Add(Pair.Key, Pair.Value);
					continue;
				}
				const FString* FingerprintA = FingerprintsA.Find(Pair.Key);
				const FString* FingerprintB = FingerprintsB.Find(Pair.Key);
				if (FingerprintA && FingerprintB && *FingerprintA != *FingerprintB)
				{
					TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
					Change->SetStringField(TEXT("id"), Pair.Key);
					Change->SetObjectField(TEXT("before"), NodesA[Pair.Key].ToSharedRef());
					Change->SetObjectField(TEXT("after"), Pair.Value.ToSharedRef());
					ChangedNodes.Add(MakeShared<FJsonValueObject>(Change));
				}
			}
			for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : NodesA)
			{
				if (!NodesB.Contains(Pair.Key))
				{
					RemovedNodes.Add(Pair.Key, Pair.Value);
				}
			}

			TMap<FString, TSharedPtr<FJsonObject>> AddedEdges;
			TMap<FString, TSharedPtr<FJsonObject>> RemovedEdges;
			for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : EdgesB)
			{
				if (!EdgesA.Contains(Pair.Key))
				{
					AddedEdges.Add(Pair.Key, Pair.Value);
				}
			}
			for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : EdgesA)
			{
				if (!EdgesB.Contains(Pair.Key))
				{
					RemovedEdges.Add(Pair.Key, Pair.Value);
				}
			}

			OutStructured->SetStringField(TEXT("contract_version"), ContractVersion);
			OutStructured->SetStringField(TEXT("asset_path_a"), AssetPathA);
			OutStructured->SetStringField(TEXT("asset_path_b"), AssetPathB);
			OutStructured->SetStringField(TEXT("graph_asset_path_a"), GraphPathA);
			OutStructured->SetStringField(TEXT("graph_asset_path_b"), GraphPathB);
			OutStructured->SetNumberField(TEXT("node_count_a"), NodesA.Num());
			OutStructured->SetNumberField(TEXT("node_count_b"), NodesB.Num());
			OutStructured->SetNumberField(TEXT("edge_count_a"), EdgesA.Num());
			OutStructured->SetNumberField(TEXT("edge_count_b"), EdgesB.Num());
			OutStructured->SetArrayField(TEXT("added_nodes"), SortedObjectValues(AddedNodes));
			OutStructured->SetArrayField(TEXT("removed_nodes"), SortedObjectValues(RemovedNodes));
			OutStructured->SetArrayField(TEXT("changed_nodes"), ChangedNodes);
			OutStructured->SetArrayField(TEXT("added_edges"), SortedObjectValues(AddedEdges));
			OutStructured->SetArrayField(TEXT("removed_edges"), SortedObjectValues(RemovedEdges));
			OutStructured->SetNumberField(TEXT("change_count"), AddedNodes.Num() + RemovedNodes.Num() + ChangedNodes.Num() + AddedEdges.Num() + RemovedEdges.Num());
			OutSummary = FString::Printf(
				TEXT("Diffed material graphs: +%d/-%d nodes, %d changed nodes, +%d/-%d edges."),
				AddedNodes.Num(),
				RemovedNodes.Num(),
				ChangedNodes.Num(),
				AddedEdges.Num(),
				RemovedEdges.Num());
			return true;
		},
		nullptr,
		0
	});

	Registry.Register({
		TEXT("material_graph_snapshot"),
		TEXT("Read-only material graph rollback snapshot with deterministic graph hash, expression nodes, and links."),
		SB::Object({
			{TEXT("asset_path"), SB::String(TEXT("UMaterial path, or a MaterialInstance whose parent is a UMaterial."))}
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
			FString GraphPath;
			if (!LoadMaterialForGraph(Context.Services, AssetPath, Material, GraphPath, OutError))
			{
				return false;
			}

			TSharedRef<FJsonObject> Snapshot = BuildMaterialGraphSnapshot(Material, AssetPath, GraphPath);
			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.material_graph_snapshot_tool.v1"));
			OutStructured->SetBoolField(TEXT("read_only"), true);
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("graph_asset_path"), GraphPath);
			OutStructured->SetStringField(TEXT("graph_hash"), Snapshot->GetStringField(TEXT("graph_hash")));
			OutStructured->SetObjectField(TEXT("snapshot"), Snapshot);
			OutSummary = FString::Printf(TEXT("Captured material graph snapshot for %s."), *GraphPath);
			return true;
		},
		nullptr,
		0
	});

	Registry.Register({
		TEXT("material_preview_compare_contract"),
		TEXT("Read-only material delivery gate for before/after preview evidence. It does not render thumbnails itself; pair asset_get_thumbnail or asset_compare receipts with material graph hashes."),
		SB::Object({
			{TEXT("asset_path"), SB::String(TEXT("Target UMaterial path, or a MaterialInstance whose parent is a UMaterial."))},
			{TEXT("before_graph_hash"), SB::String(TEXT("Optional graph hash captured before the material edit."))},
			{TEXT("after_graph_hash"), SB::String(TEXT("Optional graph hash captured after the material edit. Defaults to current hash."))},
			{TEXT("before_preview_hash"), SB::String(TEXT("Optional caller-provided hash/digest of the before preview image."))},
			{TEXT("after_preview_hash"), SB::String(TEXT("Optional caller-provided hash/digest of the after preview image."))},
			{TEXT("thumbnail_tool"), SB::String(TEXT("Preview source tool name; default asset_get_thumbnail, or asset_compare when both thumbnails are captured there."))}
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
			FString GraphPath;
			if (!LoadMaterialForGraph(Context.Services, AssetPath, Material, GraphPath, OutError))
			{
				return false;
			}

			TSharedRef<FJsonObject> CurrentSnapshot = BuildMaterialGraphSnapshot(Material, AssetPath, GraphPath);
			const FString CurrentGraphHash = CurrentSnapshot->GetStringField(TEXT("graph_hash"));
			FString BeforeGraphHash;
			FString AfterGraphHash;
			FString BeforePreviewHash;
			FString AfterPreviewHash;
			FString ThumbnailTool;
			Arguments->TryGetStringField(TEXT("before_graph_hash"), BeforeGraphHash);
			Arguments->TryGetStringField(TEXT("after_graph_hash"), AfterGraphHash);
			Arguments->TryGetStringField(TEXT("before_preview_hash"), BeforePreviewHash);
			Arguments->TryGetStringField(TEXT("after_preview_hash"), AfterPreviewHash);
			Arguments->TryGetStringField(TEXT("thumbnail_tool"), ThumbnailTool);
			if (AfterGraphHash.TrimStartAndEnd().IsEmpty())
			{
				AfterGraphHash = CurrentGraphHash;
			}
			if (ThumbnailTool.TrimStartAndEnd().IsEmpty())
			{
				ThumbnailTool = TEXT("asset_get_thumbnail");
			}

			const bool bHasGraphPair = !BeforeGraphHash.TrimStartAndEnd().IsEmpty() && !AfterGraphHash.TrimStartAndEnd().IsEmpty();
			const bool bHasPreviewPair = !BeforePreviewHash.TrimStartAndEnd().IsEmpty() && !AfterPreviewHash.TrimStartAndEnd().IsEmpty();
			const bool bGraphChanged = bHasGraphPair && BeforeGraphHash != AfterGraphHash;
			const bool bPreviewChanged = bHasPreviewPair && BeforePreviewHash != AfterPreviewHash;

			TArray<TSharedPtr<FJsonValue>> RequiredEvidence;
			RequiredEvidence.Add(MakeShared<FJsonValueString>(TEXT("material_graph_snapshot before edit")));
			RequiredEvidence.Add(MakeShared<FJsonValueString>(TEXT("material_graph_snapshot after edit")));
			RequiredEvidence.Add(MakeShared<FJsonValueString>(TEXT("asset_get_thumbnail before preview or asset_compare baseline thumbnail")));
			RequiredEvidence.Add(MakeShared<FJsonValueString>(TEXT("asset_get_thumbnail after preview or asset_compare candidate thumbnail")));
			RequiredEvidence.Add(MakeShared<FJsonValueString>(TEXT("material_recompile/material_diagnose clean diagnostics for acceptance")));

			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.material_preview_compare_contract.v1"));
			OutStructured->SetBoolField(TEXT("read_only"), true);
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("graph_asset_path"), GraphPath);
			OutStructured->SetStringField(TEXT("current_graph_hash"), CurrentGraphHash);
			OutStructured->SetStringField(TEXT("before_graph_hash"), BeforeGraphHash);
			OutStructured->SetStringField(TEXT("after_graph_hash"), AfterGraphHash);
			OutStructured->SetStringField(TEXT("before_preview_hash"), BeforePreviewHash);
			OutStructured->SetStringField(TEXT("after_preview_hash"), AfterPreviewHash);
			OutStructured->SetStringField(TEXT("thumbnail_tool"), ThumbnailTool);
			OutStructured->SetBoolField(TEXT("graph_pair_present"), bHasGraphPair);
			OutStructured->SetBoolField(TEXT("preview_pair_present"), bHasPreviewPair);
			OutStructured->SetBoolField(TEXT("graph_changed"), bGraphChanged);
			OutStructured->SetBoolField(TEXT("preview_changed"), bPreviewChanged);
			OutStructured->SetStringField(TEXT("status"), bHasPreviewPair ? TEXT("preview_comparison_ready") : TEXT("preview_comparison_pending"));
			OutStructured->SetStringField(TEXT("acceptance_boundary"), TEXT("This tool only evaluates supplied preview digests and graph hashes; it does not render or visually judge images."));
			OutStructured->SetArrayField(TEXT("required_evidence"), RequiredEvidence);
			OutSummary = bHasPreviewPair
				? FString::Printf(TEXT("Material preview compare contract ready for %s: graph_changed=%s preview_changed=%s."),
					*GraphPath,
					bGraphChanged ? TEXT("true") : TEXT("false"),
					bPreviewChanged ? TEXT("true") : TEXT("false"))
				: FString::Printf(TEXT("Material preview compare contract pending preview image hashes for %s."), *GraphPath);
			return true;
		},
		nullptr,
		0
	});

	Registry.Register({
		TEXT("material_graph_restore_snapshot"),
		TEXT("Restore a material graph from a snapshot on disposable targets. It no-ops when the hash already matches, and conservatively repairs changed graphs by removing extra nodes/edges and reconnecting snapshot links when all snapshot nodes still exist."),
		SB::Object({
			{TEXT("asset_path"), SB::String(TEXT("Target UMaterial path."))},
			{TEXT("snapshot"), SB::Object({}, {}, TEXT("Snapshot object returned by material_graph_snapshot."))},
			{TEXT("allow_disposable_write"), SB::Boolean(TEXT("Required for normal restore-check."))},
			{TEXT("allow_production_restore"), SB::Boolean(TEXT("Explicit production override; off by default."))},
			{TEXT("save"), SB::Boolean(TEXT("Save after restore; default false for smoke/rollback use."))}
		}, {TEXT("asset_path"), TEXT("snapshot")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				OutError = TEXT("Missing asset_path.");
				return false;
			}

			const bool bAllowDisposableWrite =
				Arguments->HasTypedField<EJson::Boolean>(TEXT("allow_disposable_write")) &&
				Arguments->GetBoolField(TEXT("allow_disposable_write"));
			const bool bAllowProductionRestore =
				Arguments->HasTypedField<EJson::Boolean>(TEXT("allow_production_restore")) &&
				Arguments->GetBoolField(TEXT("allow_production_restore"));
			const bool bSave =
				Arguments->HasTypedField<EJson::Boolean>(TEXT("save")) &&
				Arguments->GetBoolField(TEXT("save"));
			const bool bSafeDisposablePath = IsSafeDisposableMaterialPath(AssetPath);
			if (!bSafeDisposablePath && !bAllowProductionRestore)
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("blocked_non_disposable_target"));
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetBoolField(TEXT("safe_disposable_path"), false);
				OutError = TEXT("material_graph_restore_snapshot requires a disposable target or allow_production_restore=true.");
				return false;
			}
			if (bSafeDisposablePath && !bAllowDisposableWrite)
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("blocked_missing_allow_disposable_write"));
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetBoolField(TEXT("safe_disposable_path"), true);
				OutError = TEXT("material_graph_restore_snapshot requires allow_disposable_write=true for disposable targets.");
				return false;
			}

			const TSharedPtr<FJsonObject>* SnapshotPtr = nullptr;
			if (!Arguments->TryGetObjectField(TEXT("snapshot"), SnapshotPtr) || !SnapshotPtr || !SnapshotPtr->IsValid())
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("invalid_snapshot"));
				OutError = TEXT("Missing snapshot object.");
				return false;
			}

			FString ExpectedHash;
			(*SnapshotPtr)->TryGetStringField(TEXT("graph_hash"), ExpectedHash);
			if (ExpectedHash.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("invalid_snapshot"));
				OutStructured->SetStringField(TEXT("error_code"), TEXT("INVALID_SNAPSHOT"));
				OutError = TEXT("Snapshot graph_hash is missing.");
				return false;
			}

			UMaterial* Material = nullptr;
			FString GraphPath;
			if (!LoadMaterialForGraph(Context.Services, AssetPath, Material, GraphPath, OutError))
			{
				return false;
			}

			TSharedRef<FJsonObject> CurrentSnapshot = BuildMaterialGraphSnapshot(Material, AssetPath, GraphPath);
			const FString CurrentHash = CurrentSnapshot->GetStringField(TEXT("graph_hash"));
			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.material_graph_restore_snapshot.v1"));
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("graph_asset_path"), GraphPath);
			OutStructured->SetStringField(TEXT("pre_hash"), CurrentHash);
			OutStructured->SetStringField(TEXT("post_hash"), CurrentHash);
			OutStructured->SetStringField(TEXT("expected_hash"), ExpectedHash);
			OutStructured->SetBoolField(TEXT("allow_disposable_write"), bAllowDisposableWrite);
			OutStructured->SetBoolField(TEXT("safe_disposable_path"), bSafeDisposablePath);
			OutStructured->SetBoolField(TEXT("allow_production_restore"), bAllowProductionRestore);
			OutStructured->SetBoolField(TEXT("requested_save"), bSave);
			OutStructured->SetObjectField(TEXT("post_snapshot"), CurrentSnapshot);

			if (CurrentHash != ExpectedHash)
			{
				const bool bRestored = RestoreMaterialGraphConservative(
					Context.Services,
					Material,
					AssetPath,
					GraphPath,
					*SnapshotPtr,
					bSave,
					OutStructured,
					OutError);
				if (bRestored)
				{
					OutSummary = FString::Printf(TEXT("Restored material graph snapshot for %s."), *GraphPath);
				}
				return bRestored;
			}

			OutStructured->SetStringField(TEXT("status"), TEXT("restored"));
			OutStructured->SetStringField(TEXT("restore_mode"), TEXT("noop_hash_already_matches_snapshot"));
			OutStructured->SetBoolField(TEXT("saved"), false);
			OutSummary = FString::Printf(TEXT("Material graph snapshot already matches for %s; no-op restore accepted."), *GraphPath);
			return true;
		},
		nullptr,
		0
	});

	Registry.Register({
		TEXT("material_safe_patch"),
		TEXT("Validate or apply a stable material graph patch plan. Write mode supports set_expression_property, connect_property, connect_expression, delete_expression, create_expression, and the hardened promote_parameter subset."),
		SB::Object({
			{TEXT("asset_path"), SB::String(TEXT("Target UMaterial path."))},
			{TEXT("dry_run"), SB::Boolean(TEXT("Must be true in this implementation; default true."))},
			{TEXT("require_snapshot"), SB::Boolean(TEXT("Contract flag for future write mode; default true."))},
			{TEXT("operations"), SB::Array(PatchOperationInputSchema(), TEXT("Patch operations using material.graph_patch.v1 schema."))}
		}, {TEXT("asset_path"), TEXT("operations")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				OutError = TEXT("Missing asset_path.");
				return false;
			}

			const bool bDryRun = !Arguments->HasTypedField<EJson::Boolean>(TEXT("dry_run")) || Arguments->GetBoolField(TEXT("dry_run"));
			const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
			if (!Arguments->TryGetArrayField(TEXT("operations"), Operations) || !Operations)
			{
				OutError = TEXT("Missing operations array.");
				return false;
			}

			UMaterial* Material = nullptr;
			FString GraphPath;
			if (!LoadMaterialForGraph(Context.Services, AssetPath, Material, GraphPath, OutError))
			{
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> OperationReceipts;
			int32 InvalidCount = 0;
			for (int32 Index = 0; Index < Operations->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Operation = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
				TSharedRef<FJsonObject> Receipt = ValidatePatchOperation(Operation, Index);
				const bool bValid = Receipt->GetBoolField(TEXT("valid"));
				if (!bValid)
				{
					++InvalidCount;
				}
				Receipt->SetBoolField(TEXT("applied"), false);
				Receipt->SetStringField(TEXT("status"), bValid ? TEXT("planned") : TEXT("rejected"));
				OperationReceipts.Add(MakeShared<FJsonValueObject>(Receipt));
			}

			OutStructured->SetStringField(TEXT("contract_version"), ContractVersion);
			OutStructured->SetStringField(TEXT("implementation_level"), TEXT("write_subset_v1_create_promote_min"));
			OutStructured->SetObjectField(TEXT("operation_schema"), MakePatchOperationSchemaJson());
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("graph_asset_path"), GraphPath);
			OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
			OutStructured->SetNumberField(TEXT("operation_count"), Operations->Num());
			OutStructured->SetNumberField(TEXT("invalid_count"), InvalidCount);
			OutStructured->SetBoolField(TEXT("preview_required_before_commit"), !bDryRun);
			OutStructured->SetStringField(TEXT("rollback_strategy"), TEXT("editor_transaction_no_save_on_failure"));
			OutStructured->SetObjectField(TEXT("snapshot_evidence"), MakeEvidenceReceipt(
				TEXT("snapshot"),
				TEXT("captured"),
				FString::Printf(TEXT("Loaded %d material expression(s) before patch validation."), Material->GetExpressions().Num())));
			OutStructured->SetObjectField(TEXT("diff_evidence"), MakeEvidenceReceipt(
				TEXT("diff"),
				bDryRun ? TEXT("planned") : TEXT("captured_after_apply"),
				bDryRun ? TEXT("Dry-run validates operations; write mode reports node/edge deltas.") : TEXT("Write mode reports before/after graph deltas.")));
			OutStructured->SetObjectField(TEXT("compile_evidence"), MakeEvidenceReceipt(
				TEXT("compile"),
				bDryRun ? TEXT("planned") : TEXT("enforced_by_receipt_gate"),
				bDryRun ? TEXT("Dry-run does not mutate; write mode enforces recompile/statistics/snapshot readback.") : TEXT("Write mode enforces material recompile/statistics/snapshot readback before completed receipts.")));
			OutStructured->SetObjectField(TEXT("preview_evidence"), MakeEvidenceReceipt(
				TEXT("preview"),
				!bDryRun ? TEXT("required_before_commit") : TEXT("planned"),
				TEXT("Capture before/after material preview evidence for production visual acceptance.")));
			OutStructured->SetObjectField(TEXT("rollback_evidence"), MakeEvidenceReceipt(
				TEXT("rollback"),
				bDryRun ? TEXT("not_required_dry_run") : TEXT("transaction_available"),
				TEXT("Write mode runs inside a scoped editor transaction; save happens after all operations apply.")));

			if (InvalidCount > 0)
			{
				OutStructured->SetBoolField(TEXT("applied"), false);
				OutStructured->SetStringField(TEXT("status"), TEXT("invalid"));
				OutStructured->SetArrayField(TEXT("operation_receipts"), OperationReceipts);
				OutError = TEXT("material_safe_patch rejected invalid operations.");
				return false;
			}

			if (bDryRun)
			{
				OutStructured->SetBoolField(TEXT("applied"), false);
				OutStructured->SetStringField(TEXT("status"), TEXT("dry_run_ok"));
				OutStructured->SetArrayField(TEXT("operation_receipts"), OperationReceipts);
				OutSummary = FString::Printf(
					TEXT("Validated material patch plan for %s: %d operations, applied=false."),
					*GraphPath,
					Operations->Num());
				return true;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialSafePatch", "SOMOLMCP Material Safe Patch"));
			Material->Modify();
			TMap<FString, TSharedPtr<FJsonObject>> BeforeNodes;
			TMap<FString, FString> BeforeFingerprints;
			TMap<FString, TSharedPtr<FJsonObject>> BeforeEdges;
			BuildGraphMaps(Material, BeforeNodes, BeforeFingerprints, BeforeEdges);
			OutStructured->SetNumberField(TEXT("before_node_count"), BeforeNodes.Num());
			OutStructured->SetNumberField(TEXT("before_edge_count"), BeforeEdges.Num());
			int32 AppliedCount = 0;
			TArray<TSharedPtr<FJsonValue>> AppliedReceipts;
			for (int32 Index = 0; Index < Operations->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Operation = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
				TSharedRef<FJsonObject> Receipt = ValidatePatchOperation(Operation, Index);
				FString ApplyError;
				const bool bApplied = ApplyPatchOperation(Context.Services, Material, Operation, Receipt, ApplyError);
				Receipt->SetBoolField(TEXT("applied"), bApplied);
				Receipt->SetStringField(TEXT("status"), bApplied ? TEXT("applied") : TEXT("failed"));
				if (!bApplied)
				{
					Receipt->SetStringField(TEXT("error"), ApplyError);
					OutStructured->SetBoolField(TEXT("applied"), false);
					OutStructured->SetStringField(TEXT("status"), TEXT("partial_or_failed"));
					OutStructured->SetNumberField(TEXT("applied_count"), AppliedCount);
					OutStructured->SetBoolField(TEXT("saved"), false);
					OutStructured->SetBoolField(TEXT("rollback_required"), AppliedCount > 0);
					OutStructured->SetStringField(TEXT("rollback_receipt"), AppliedCount > 0 ? TEXT("transaction_open_asset_not_saved") : TEXT("no_mutation_committed"));
					AppliedReceipts.Add(MakeShared<FJsonValueObject>(Receipt));
					OutStructured->SetArrayField(TEXT("operation_receipts"), AppliedReceipts);
					OutError = ApplyError;
					return false;
				}
				++AppliedCount;
				AppliedReceipts.Add(MakeShared<FJsonValueObject>(Receipt));
			}

			Material->PostEditChange();
			Material->MarkPackageDirty();
			TMap<FString, TSharedPtr<FJsonObject>> AfterNodes;
			TMap<FString, FString> AfterFingerprints;
			TMap<FString, TSharedPtr<FJsonObject>> AfterEdges;
			BuildGraphMaps(Material, AfterNodes, AfterFingerprints, AfterEdges);
			if (!RunMaterialMutationGate(AssetPath, GraphPath, Material, OutStructured, OutError))
			{
				OutStructured->SetBoolField(TEXT("applied"), true);
				OutStructured->SetStringField(TEXT("status"), TEXT("failed_validation"));
				OutStructured->SetBoolField(TEXT("saved"), false);
				OutStructured->SetStringField(TEXT("rollback_receipt"), TEXT("transaction_open_asset_not_saved"));
				OutStructured->SetArrayField(TEXT("operation_receipts"), AppliedReceipts);
				return false;
			}
			FString SaveError;
			const bool bSaved = Context.Services.SaveAsset(AssetPath, false, SaveError);
			OutStructured->SetBoolField(TEXT("applied"), true);
			OutStructured->SetBoolField(TEXT("saved"), bSaved);
			OutStructured->SetStringField(TEXT("status"), bSaved ? TEXT("applied") : TEXT("applied_save_failed"));
			OutStructured->SetNumberField(TEXT("applied_count"), AppliedCount);
			OutStructured->SetNumberField(TEXT("after_node_count"), AfterNodes.Num());
			OutStructured->SetNumberField(TEXT("after_edge_count"), AfterEdges.Num());
			OutStructured->SetNumberField(TEXT("node_count_delta"), AfterNodes.Num() - BeforeNodes.Num());
			OutStructured->SetNumberField(TEXT("edge_count_delta"), AfterEdges.Num() - BeforeEdges.Num());
			OutStructured->SetStringField(TEXT("rollback_receipt"), bSaved ? TEXT("transaction_available_until_editor_undo_stack_expires") : TEXT("applied_but_save_failed"));
			OutStructured->SetArrayField(TEXT("operation_receipts"), AppliedReceipts);
			if (!bSaved)
			{
				OutError = SaveError.IsEmpty() ? TEXT("Patch applied but SaveAsset failed.") : SaveError;
				return false;
			}
			OutSummary = FString::Printf(
				TEXT("Applied material patch for %s: %d operations."),
				*GraphPath,
				AppliedCount);
			return true;
		},
		nullptr,
		0
	});

	Registry.Register({
		TEXT("material_instance_promote_parameter"),
		TEXT("Prepare or apply a stable promote_parameter patch receipt for a material expression. Write mode uses the same hardened subset as material_safe_patch."),
		SB::Object({
			{TEXT("asset_path"), SB::String(TEXT("Target UMaterial or MaterialInstance path. MaterialInstance resolves to its parent graph."))},
			{TEXT("expression_guid"), SB::String(TEXT("Target expression GUID."))},
			{TEXT("expression_index"), SB::Integer(TEXT("Fallback target expression index."))},
			{TEXT("parameter_name"), SB::String(TEXT("Parameter name to expose."))},
			{TEXT("parameter_type"), SB::String(TEXT("Optional parameter type hint: scalar, vector, texture, static_switch."))},
			{TEXT("dry_run"), SB::Boolean(TEXT("Must be true in this implementation; default true."))}
		}, {TEXT("asset_path"), TEXT("parameter_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			FString ParameterName;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
				!Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName) ||
				ParameterName.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("Missing asset_path or parameter_name.");
				return false;
			}

			const bool bDryRun = !Arguments->HasTypedField<EJson::Boolean>(TEXT("dry_run")) || Arguments->GetBoolField(TEXT("dry_run"));

			UMaterial* Material = nullptr;
			FString GraphPath;
			if (!LoadMaterialForGraph(Context.Services, AssetPath, Material, GraphPath, OutError))
			{
				return false;
			}

			FString TargetId;
			UMaterialExpression* Expression = ResolveExpressionTarget(Material, Arguments, TargetId, OutError);
			if (!Expression)
			{
				return false;
			}

			FString ParameterType;
			Arguments->TryGetStringField(TEXT("parameter_type"), ParameterType);

			TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
			Operation->SetStringField(TEXT("op"), TEXT("promote_parameter"));
			Operation->SetStringField(TEXT("operation_id"), FString::Printf(TEXT("promote_%s"), *TargetId));
			Operation->SetStringField(TEXT("expression_guid"), Expression->MaterialExpressionGuid.ToString());
			Operation->SetNumberField(TEXT("expression_index"), FindExpressionIndex(Material, Expression));
			Operation->SetStringField(TEXT("parameter_name"), ParameterName);
			if (!ParameterType.TrimStartAndEnd().IsEmpty())
			{
				Operation->SetStringField(TEXT("parameter_type"), ParameterType);
			}

			TSharedRef<FJsonObject> Receipt = ValidatePatchOperation(Operation, 0);
			Receipt->SetBoolField(TEXT("applied"), false);
			Receipt->SetStringField(TEXT("status"), Receipt->GetBoolField(TEXT("valid")) ? TEXT("planned") : TEXT("rejected"));

			OutStructured->SetStringField(TEXT("contract_version"), ContractVersion);
			OutStructured->SetStringField(TEXT("implementation_level"), TEXT("write_subset_v1_create_promote_min"));
			OutStructured->SetObjectField(TEXT("operation_schema"), MakePatchOperationSchemaJson());
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("graph_asset_path"), GraphPath);
			OutStructured->SetStringField(TEXT("target_expression_id"), TargetId);
			OutStructured->SetStringField(TEXT("target_expression_class"), ShortClassName(Expression));
			OutStructured->SetStringField(TEXT("parameter_name"), ParameterName);
			OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
			OutStructured->SetBoolField(TEXT("applied"), false);
			OutStructured->SetObjectField(TEXT("planned_operation"), Operation);
			OutStructured->SetObjectField(TEXT("operation_receipt"), Receipt);
			OutStructured->SetStringField(TEXT("rollback_strategy"), TEXT("editor_transaction_no_save_on_failure"));

			if (bDryRun)
			{
				OutStructured->SetStringField(TEXT("status"), Receipt->GetBoolField(TEXT("valid")) ? TEXT("dry_run_ok") : TEXT("dry_run_invalid"));
				OutSummary = FString::Printf(
					TEXT("Prepared promote_parameter patch for %s as '%s' on %s; applied=false."),
					*TargetId,
					*ParameterName,
					*GraphPath);
				return Receipt->GetBoolField(TEXT("valid"));
			}

			if (!Receipt->GetBoolField(TEXT("valid")))
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("invalid"));
				OutError = TEXT("material_instance_promote_parameter rejected invalid operation.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialPromoteParameter", "SOMOLMCP Promote Material Parameter"));
			Material->Modify();
			FString ApplyError;
			const bool bApplied = ApplyPromoteParameter(Material, Operation, Receipt, ApplyError);
			Receipt->SetBoolField(TEXT("applied"), bApplied);
			Receipt->SetStringField(TEXT("status"), bApplied ? TEXT("applied") : TEXT("failed"));
			OutStructured->SetObjectField(TEXT("operation_receipt"), Receipt);
			if (!bApplied)
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("failed"));
				OutStructured->SetStringField(TEXT("rollback_receipt"), TEXT("transaction_open_asset_not_saved"));
				OutError = ApplyError;
				return false;
			}

			Material->PostEditChange();
			Material->MarkPackageDirty();
			if (!RunMaterialMutationGate(AssetPath, GraphPath, Material, OutStructured, OutError))
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("failed_validation"));
				OutStructured->SetStringField(TEXT("rollback_receipt"), TEXT("transaction_open_asset_not_saved"));
				return false;
			}
			FString SaveError;
			const bool bSaved = Context.Services.SaveAsset(AssetPath, false, SaveError);
			OutStructured->SetBoolField(TEXT("applied"), true);
			OutStructured->SetBoolField(TEXT("saved"), bSaved);
			OutStructured->SetStringField(TEXT("status"), bSaved ? TEXT("applied") : TEXT("applied_save_failed"));
			OutStructured->SetStringField(TEXT("rollback_receipt"), bSaved ? TEXT("transaction_available_until_editor_undo_stack_expires") : TEXT("applied_but_save_failed"));
			if (!bSaved)
			{
				OutError = SaveError.IsEmpty() ? TEXT("Promote applied but SaveAsset failed.") : SaveError;
				return false;
			}
			OutSummary = FString::Printf(
				TEXT("Applied promote_parameter patch for %s as '%s' on %s."),
				*TargetId,
				*ParameterName,
				*GraphPath);
			return true;
		},
		nullptr,
		0
	});

	Registry.Register({
		TEXT("material_disconnect_property"),
		TEXT("Disconnect a material property input with C++ readback verification, graph hash evidence, and optional save."),
		SB::Object({
			{TEXT("asset_path"), SB::String(TEXT("Target UMaterial path, or a MaterialInstance whose parent is a UMaterial."))},
			{TEXT("property"), SB::String(TEXT("Material property name, e.g. BaseColor, Roughness, Normal."))},
			{TEXT("save_asset"), SB::Boolean(TEXT("Save after disconnect; default true."))}
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
			FString GraphPath;
			if (!LoadMaterialForGraph(Context.Services, AssetPath, Material, GraphPath, OutError))
			{
				return false;
			}

			EMaterialProperty Property = MP_BaseColor;
			if (!TryParseMaterialProperty(PropertyName, Property, Material))
			{
				OutError = FString::Printf(TEXT("Unsupported material property '%s'."), *PropertyName);
				return false;
			}

			TSharedRef<FJsonObject> BeforeSnapshot = BuildMaterialGraphSnapshot(Material, AssetPath, GraphPath);
			FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
			UMaterialExpression* BeforeExpression = Input ? Input->Expression : nullptr;
			const int32 BeforeExpressionIndex = FindExpressionIndex(Material, BeforeExpression);
			const FString BeforeExpressionId = ExpressionId(Material, BeforeExpression);
			const int32 BeforeOutputIndex = Input ? Input->OutputIndex : INDEX_NONE;
			const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;

			if (Input && Input->Expression)
			{
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialDisconnectPropertyVerified", "SOMOLMCP Disconnect Material Property Verified"));
				Material->Modify();
				Input->Expression = nullptr;
				Input->OutputIndex = 0;
				Material->PostEditChange();
				Material->MarkPackageDirty();
			}

			FString SaveError;
			const bool bSaved = bSaveAsset ? Context.Services.SaveAsset(AssetPath, false, SaveError) : false;
			if (bSaveAsset && !bSaved)
			{
				OutError = SaveError.IsEmpty() ? TEXT("Property disconnected but SaveAsset failed.") : SaveError;
				return false;
			}

			TSharedRef<FJsonObject> AfterSnapshot = BuildMaterialGraphSnapshot(Material, AssetPath, GraphPath);
			FExpressionInput* AfterInput = Material->GetExpressionInputForProperty(Property);
			const bool bVerified = !AfterInput || AfterInput->Expression == nullptr;

			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.material_disconnect_property.v2"));
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("graph_asset_path"), GraphPath);
			OutStructured->SetStringField(TEXT("property"), PropertyName);
			OutStructured->SetStringField(TEXT("before_hash"), BeforeSnapshot->GetStringField(TEXT("graph_hash")));
			OutStructured->SetStringField(TEXT("after_hash"), AfterSnapshot->GetStringField(TEXT("graph_hash")));
			OutStructured->SetBoolField(TEXT("had_connection"), BeforeExpression != nullptr);
			OutStructured->SetStringField(TEXT("before_expression_id"), BeforeExpressionId);
			OutStructured->SetNumberField(TEXT("before_expression_index"), BeforeExpressionIndex);
			OutStructured->SetNumberField(TEXT("before_output_index"), BeforeOutputIndex);
			OutStructured->SetBoolField(TEXT("disconnected"), BeforeExpression != nullptr);
			OutStructured->SetBoolField(TEXT("connection_verified"), bVerified);
			OutStructured->SetBoolField(TEXT("saved"), bSaveAsset ? bSaved : false);
			OutStructured->SetStringField(TEXT("status"), bVerified ? (BeforeExpression ? TEXT("disconnected") : TEXT("noop_not_connected")) : TEXT("failed_readback"));
			OutStructured->SetObjectField(TEXT("before_snapshot"), BeforeSnapshot);
			OutStructured->SetObjectField(TEXT("after_snapshot"), AfterSnapshot);
			AttachMaterialGraphEditReceipt(
				OutStructured,
				AssetPath,
				GraphPath,
				Material,
				BeforeSnapshot,
				AfterSnapshot,
				TEXT("disconnect_property"),
				TEXT("connection_verified"),
				bVerified);

			if (!bVerified)
			{
				OutError = TEXT("Material property disconnect readback failed.");
				return false;
			}
			if (!RunMaterialMutationGate(AssetPath, GraphPath, Material, OutStructured, OutError))
			{
				return false;
			}

			OutSummary = BeforeExpression
				? FString::Printf(TEXT("Disconnected material property %s on %s."), *PropertyName, *GraphPath)
				: FString::Printf(TEXT("Material property %s on %s was already disconnected."), *PropertyName, *GraphPath);
			return true;
		},
		nullptr,
		0
	});

	Registry.Register({
		TEXT("material_disconnect_expressions"),
		TEXT("Disconnect a material expression input link with C++ readback verification, graph hash evidence, and optional save."),
		SB::Object({
			{TEXT("asset_path"), SB::String(TEXT("Target UMaterial path, or a MaterialInstance whose parent is a UMaterial."))},
			{TEXT("output_expression_id"), SB::Integer(TEXT("Existing schema: source expression index."))},
			{TEXT("input_expression_id"), SB::Integer(TEXT("Existing schema: target expression index."))},
			{TEXT("input_name"), SB::String(TEXT("Target input pin name. If omitted, the first input linked to the source expression is used."))},
			{TEXT("from_expression_index"), SB::Integer(TEXT("Alias for output_expression_id."))},
			{TEXT("from_expression_guid"), SB::String(TEXT("Source expression GUID."))},
			{TEXT("to_expression_index"), SB::Integer(TEXT("Alias for input_expression_id."))},
			{TEXT("to_expression_guid"), SB::String(TEXT("Target expression GUID."))},
			{TEXT("save_asset"), SB::Boolean(TEXT("Save after disconnect; default true."))}
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
			FString GraphPath;
			if (!LoadMaterialForGraph(Context.Services, AssetPath, Material, GraphPath, OutError))
			{
				return false;
			}

			TSharedRef<FJsonObject> SourceArgs = MakeShared<FJsonObject>();
			FString FromGuid;
			if (Arguments->TryGetStringField(TEXT("from_expression_guid"), FromGuid) && !FromGuid.IsEmpty())
			{
				SourceArgs->SetStringField(TEXT("expression_guid"), FromGuid);
			}
			else
			{
				int32 FromIndex = INDEX_NONE;
				if (!Arguments->TryGetNumberField(TEXT("from_expression_index"), FromIndex))
				{
					Arguments->TryGetNumberField(TEXT("output_expression_id"), FromIndex);
				}
				if (FromIndex != INDEX_NONE)
				{
					SourceArgs->SetNumberField(TEXT("expression_index"), FromIndex);
				}
			}

			TSharedRef<FJsonObject> TargetArgs = MakeShared<FJsonObject>();
			FString ToGuid;
			if (Arguments->TryGetStringField(TEXT("to_expression_guid"), ToGuid) && !ToGuid.IsEmpty())
			{
				TargetArgs->SetStringField(TEXT("expression_guid"), ToGuid);
			}
			else
			{
				int32 ToIndex = INDEX_NONE;
				if (!Arguments->TryGetNumberField(TEXT("to_expression_index"), ToIndex))
				{
					Arguments->TryGetNumberField(TEXT("input_expression_id"), ToIndex);
				}
				if (ToIndex != INDEX_NONE)
				{
					TargetArgs->SetNumberField(TEXT("expression_index"), ToIndex);
				}
			}

			FString FromId;
			FString ToId;
			UMaterialExpression* FromExpression = ResolveExpressionTarget(Material, SourceArgs, FromId, OutError);
			if (!FromExpression)
			{
				return false;
			}
			UMaterialExpression* ToExpression = ResolveExpressionTarget(Material, TargetArgs, ToId, OutError);
			if (!ToExpression)
			{
				return false;
			}

			FString InputName;
			Arguments->TryGetStringField(TEXT("input_name"), InputName);
			FExpressionInput* Input = FindExpressionInputByName(ToExpression, InputName);
			if (!Input)
			{
				for (FExpressionInputIterator InputIt{ToExpression}; InputIt; ++InputIt)
				{
					if (InputIt.Input && InputIt.Input->Expression == FromExpression)
					{
						Input = InputIt.Input;
						InputName = ToExpression->GetInputName(InputIt.Index).ToString();
						break;
					}
				}
			}
			if (!Input)
			{
				OutError = InputName.TrimStartAndEnd().IsEmpty()
					? TEXT("No input pin linked to the source expression was found.")
					: FString::Printf(TEXT("Input pin '%s' was not found on target expression."), *InputName);
				return false;
			}
			if (Input->Expression != FromExpression)
			{
				OutStructured->SetStringField(TEXT("schema"), TEXT("somol.material_disconnect_expressions.v2"));
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("graph_asset_path"), GraphPath);
				OutStructured->SetStringField(TEXT("from_expression_id"), FromId);
				OutStructured->SetStringField(TEXT("to_expression_id"), ToId);
				OutStructured->SetStringField(TEXT("input_name"), InputName);
				OutStructured->SetBoolField(TEXT("linked_to_requested_source"), false);
				if (Input->Expression)
				{
					OutStructured->SetStringField(TEXT("actual_source_expression_id"), ExpressionId(Material, Input->Expression));
					OutStructured->SetNumberField(TEXT("actual_source_expression_index"), FindExpressionIndex(Material, Input->Expression));
				}
				OutError = TEXT("Target input is not linked to the requested source expression.");
				return false;
			}

			TSharedRef<FJsonObject> BeforeSnapshot = BuildMaterialGraphSnapshot(Material, AssetPath, GraphPath);
			const int32 BeforeOutputIndex = Input->OutputIndex;
			const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;

			{
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialDisconnectExpressionsVerified", "SOMOLMCP Disconnect Material Expressions Verified"));
				ToExpression->Modify();
				Input->Expression = nullptr;
				Input->OutputIndex = 0;
				Material->PostEditChange();
				Material->MarkPackageDirty();
			}

			FString SaveError;
			const bool bSaved = bSaveAsset ? Context.Services.SaveAsset(AssetPath, false, SaveError) : false;
			if (bSaveAsset && !bSaved)
			{
				OutError = SaveError.IsEmpty() ? TEXT("Expression link disconnected but SaveAsset failed.") : SaveError;
				return false;
			}

			TSharedRef<FJsonObject> AfterSnapshot = BuildMaterialGraphSnapshot(Material, AssetPath, GraphPath);
			FExpressionInput* AfterInput = FindExpressionInputByName(ToExpression, InputName);
			const bool bVerified = !AfterInput || AfterInput->Expression != FromExpression;

			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.material_disconnect_expressions.v2"));
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("graph_asset_path"), GraphPath);
			OutStructured->SetStringField(TEXT("from_expression_id"), FromId);
			OutStructured->SetNumberField(TEXT("from_expression_index"), FindExpressionIndex(Material, FromExpression));
			OutStructured->SetStringField(TEXT("to_expression_id"), ToId);
			OutStructured->SetNumberField(TEXT("to_expression_index"), FindExpressionIndex(Material, ToExpression));
			OutStructured->SetStringField(TEXT("input_name"), InputName);
			OutStructured->SetNumberField(TEXT("before_output_index"), BeforeOutputIndex);
			OutStructured->SetStringField(TEXT("before_hash"), BeforeSnapshot->GetStringField(TEXT("graph_hash")));
			OutStructured->SetStringField(TEXT("after_hash"), AfterSnapshot->GetStringField(TEXT("graph_hash")));
			OutStructured->SetBoolField(TEXT("disconnected"), true);
			OutStructured->SetBoolField(TEXT("connection_verified"), bVerified);
			OutStructured->SetBoolField(TEXT("saved"), bSaveAsset ? bSaved : false);
			OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("disconnected") : TEXT("failed_readback"));
			OutStructured->SetObjectField(TEXT("before_snapshot"), BeforeSnapshot);
			OutStructured->SetObjectField(TEXT("after_snapshot"), AfterSnapshot);
			AttachMaterialGraphEditReceipt(
				OutStructured,
				AssetPath,
				GraphPath,
				Material,
				BeforeSnapshot,
				AfterSnapshot,
				TEXT("disconnect_expressions"),
				TEXT("connection_verified"),
				bVerified);

			if (!bVerified)
			{
				OutError = TEXT("Material expression disconnect readback failed.");
				return false;
			}
			if (!RunMaterialMutationGate(AssetPath, GraphPath, Material, OutStructured, OutError))
			{
				return false;
			}

			OutSummary = FString::Printf(TEXT("Disconnected expression %s from %s.%s on %s."), *FromId, *ToId, *InputName, *GraphPath);
			return true;
		},
		nullptr,
		0
	});
}
}
