// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP v3.8 — MegaWorld Tools (ported to new ToolRegistry API)
// Tools:
//   world_mpc_weather_override       — Override a global MPC scalar parameter at runtime
//   landscape_hole_punch             - safe visibility cutout fallback
//   swarm_virtual_detachment_mock    — Mock: peel a member from a swarm and report spawn

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpWriteFlush.h"
#include "SOMOLMCP.h"

#include "Landscape.h"
#include "LandscapeComponent.h"
#include "CoreMinimal.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "ScopedTransaction.h"

namespace UE::SOMOLMCP
{
	namespace
	{
		static bool RunWorldMpcWeatherOverride(
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			FString CollectionPath;
			FString ParameterName;
			double ScalarValue = 0.0;

			if (!Arguments->TryGetStringField(TEXT("collection_path"), CollectionPath)
				|| !Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName)
				|| !Arguments->TryGetNumberField(TEXT("scalar_value"), ScalarValue))
			{
				OutError = TEXT("Missing required arguments: collection_path, parameter_name, scalar_value.");
				return false;
			}

			UObject* Loaded = Context.Services.LoadAsset(CollectionPath, OutError);
			UMaterialParameterCollection* Mpc = Cast<UMaterialParameterCollection>(Loaded);
			if (!Mpc)
			{
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(TEXT("Asset at '%s' is not a UMaterialParameterCollection."), *CollectionPath);
				}
				return false;
			}

			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				OutError = TEXT("No editor world available.");
				return false;
			}

			UMaterialParameterCollectionInstance* Inst = World->GetParameterCollectionInstance(Mpc);
			if (!Inst || !Inst->SetScalarParameterValue(FName(*ParameterName), static_cast<float>(ScalarValue)))
			{
				OutError = FString::Printf(TEXT("Failed to set scalar '%s' on MPC '%s'."), *ParameterName, *CollectionPath);
				return false;
			}
			float ReadbackValue = 0.0f;
			if (!Inst->GetScalarParameterValue(FName(*ParameterName), ReadbackValue)
				|| !FMath::IsNearlyEqual(ReadbackValue, static_cast<float>(ScalarValue), 0.0001f))
			{
				OutStructured->SetNumberField(TEXT("requested_scalar_value"), ScalarValue);
				OutStructured->SetNumberField(TEXT("readback_scalar_value"), ReadbackValue);
				OutError = FString::Printf(TEXT("MPC scalar '%s' set call succeeded but readback did not match."), *ParameterName);
				return false;
			}

			OutStructured->SetBoolField(TEXT("success"), true);
			OutStructured->SetStringField(TEXT("collection_path"), CollectionPath);
			OutStructured->SetStringField(TEXT("parameter_name"), ParameterName);
			OutStructured->SetNumberField(TEXT("scalar_value"), ScalarValue);
			OutStructured->SetNumberField(TEXT("readback_scalar_value"), ReadbackValue);
			OutSummary = FString::Printf(TEXT("MPC '%s' param '%s' set to %.4f"), *CollectionPath, *ParameterName, ScalarValue);
			return true;
		}

		static bool ParseRegionBox(const TSharedRef<FJsonObject>& Arguments, FBox2D& OutRegion, FString& OutError)
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (!Arguments->TryGetArrayField(TEXT("region_box"), Arr) || !Arr || Arr->Num() < 4)
			{
				OutError = TEXT("Missing or invalid region_box; expected [x_min,y_min,x_max,y_max].");
				return false;
			}
			const double X0 = (*Arr)[0]->AsNumber();
			const double Y0 = (*Arr)[1]->AsNumber();
			const double X1 = (*Arr)[2]->AsNumber();
			const double Y1 = (*Arr)[3]->AsNumber();
			OutRegion = FBox2D(
				FVector2D(FMath::Min(X0, X1), FMath::Min(Y0, Y1)),
				FVector2D(FMath::Max(X0, X1), FMath::Max(Y0, Y1)));
			return true;
		}

		static ALandscape* ResolveLandscapeById(UWorld* World, const FString& LandscapeName)
		{
			if (!World) return nullptr;
			for (TActorIterator<ALandscape> It(World); It; ++It)
			{
				ALandscape* Landscape = *It;
				if (Landscape
					&& (Landscape->GetPathName() == LandscapeName
						|| Landscape->GetName() == LandscapeName
						|| Landscape->GetActorLabel() == LandscapeName))
				{
					return Landscape;
				}
			}
			return nullptr;
		}

		static bool RunLandscapeHolePunch(
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			FString LandscapeName;
			if (!Arguments->TryGetStringField(TEXT("landscape_name"), LandscapeName))
			{
				OutError = TEXT("Missing landscape_name.");
				return false;
			}

			FBox2D Region;
			if (!ParseRegionBox(Arguments, Region, OutError))
			{
				OutStructured->SetBoolField(TEXT("success"), false);
				OutStructured->SetStringField(TEXT("error_code"), TEXT("INVALID_TYPE"));
				return false;
			}

			UWorld* World = Context.Services.GetEditorWorld(OutError);
			if (!World)
			{
				return false;
			}

			ALandscape* Landscape = ResolveLandscapeById(World, LandscapeName);
			if (!Landscape)
			{
				OutStructured->SetBoolField(TEXT("success"), false);
				OutStructured->SetStringField(TEXT("error_code"), TEXT("NOT_FOUND"));
				OutStructured->SetStringField(TEXT("landscape_name"), LandscapeName);
				OutError = FString::Printf(TEXT("Landscape '%s' not found."), *LandscapeName);
				return false;
			}

			const bool bVisible = Arguments->HasTypedField<EJson::Boolean>(TEXT("visible"))
				? Arguments->GetBoolField(TEXT("visible"))
				: false;

			const FScopedTransaction Tx(NSLOCTEXT("SOMOLMCP", "LandscapeHolePunchVisibilityFallback", "SOMOLMCP Landscape Hole Punch Visibility Fallback"));
			Landscape->Modify();
			int32 Affected = 0;
			for (ULandscapeComponent* Component : Landscape->LandscapeComponents)
			{
				if (!Component) continue;
				const FVector Center = Component->Bounds.Origin;
				if (!Region.IsInside(FVector2D(Center.X, Center.Y))) continue;

				Component->Modify();
				Component->SetVisibility(bVisible);
				Component->SetHiddenInGame(!bVisible);
				Component->MarkRenderStateDirty();
				++Affected;
			}

			SololmcpWriteFlush::EnsureFlushed(Landscape);

			OutStructured->SetBoolField(TEXT("success"), Affected > 0);
			OutStructured->SetStringField(TEXT("landscape_name"), LandscapeName);
			OutStructured->SetStringField(TEXT("landscape_actor"), Landscape->GetName());
			OutStructured->SetBoolField(TEXT("visible"), bVisible);
			OutStructured->SetNumberField(TEXT("component_count_affected"), Affected);
			TArray<TSharedPtr<FJsonValue>> RegionJson;
			RegionJson.Add(MakeShared<FJsonValueNumber>(Region.Min.X));
			RegionJson.Add(MakeShared<FJsonValueNumber>(Region.Min.Y));
			RegionJson.Add(MakeShared<FJsonValueNumber>(Region.Max.X));
			RegionJson.Add(MakeShared<FJsonValueNumber>(Region.Max.Y));
			OutStructured->SetArrayField(TEXT("region_box"), RegionJson);
			OutStructured->SetStringField(TEXT("fallback_mode"), TEXT("component_visibility"));

			if (Affected == 0)
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("NO_MATCH"));
				OutError = TEXT("No landscape components had centers inside region_box.");
				OutSummary = FString::Printf(TEXT("landscape_hole_punch matched 0 components on '%s'."), *LandscapeName);
				return false;
			}

			OutSummary = FString::Printf(TEXT("landscape_hole_punch visibility fallback set visible=%s on %d components."),
				bVisible ? TEXT("true") : TEXT("false"), Affected);
			return true;
		}

		static bool RunSwarmVirtualDetachmentMock(
			const FSololmcpToolExecutionContext& /*Context*/,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			int64 SwarmNodeId = 0;
			int64 MemberIndex = 0;
			double Tmp = 0.0;
			if (Arguments->TryGetNumberField(TEXT("swarm_node_id"), Tmp)) { SwarmNodeId = static_cast<int64>(Tmp); }
			else { OutError = TEXT("Missing swarm_node_id."); return false; }
			if (Arguments->TryGetNumberField(TEXT("member_index"), Tmp)) { MemberIndex = static_cast<int64>(Tmp); }
			else { OutError = TEXT("Missing member_index."); return false; }

			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("error_code"), TEXT("MOCK_ONLY"));
			OutStructured->SetStringField(TEXT("event"), TEXT("No UE entity was spawned or detached"));
			OutStructured->SetNumberField(TEXT("swarm_node_id"), static_cast<double>(SwarmNodeId));
			OutStructured->SetNumberField(TEXT("member_index"), static_cast<double>(MemberIndex));
			OutError = TEXT("swarm_virtual_detachment_mock is diagnostic-only and does not mutate the world.");
			OutSummary = FString::Printf(TEXT("Swarm detach mock rejected as non-mutating: node=%lld member=%lld"), SwarmNodeId, MemberIndex);
			return false;
		}
	}

	void RegisterMegaWorldTools(FSololmcpToolRegistry& Registry)
	{
		Registry.Register({
			TEXT("world_mpc_weather_override"),
			TEXT("Override a scalar parameter on a global MaterialParameterCollection instance (e.g. snow coverage, "
			     "rain wetness) at editor runtime. Applies to the active editor world."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("collection_path"), FSololmcpSchemaBuilder::String(TEXT("/Game path of the UMaterialParameterCollection asset"))},
					{TEXT("parameter_name"),  FSololmcpSchemaBuilder::String(TEXT("Scalar parameter name inside the MPC"))},
					{TEXT("scalar_value"),    FSololmcpSchemaBuilder::Number(TEXT("New scalar value"))},
				},
				{TEXT("collection_path"), TEXT("parameter_name"), TEXT("scalar_value")}),
			&RunWorldMpcWeatherOverride
		});

		Registry.Register({
			TEXT("landscape_hole_punch"),
			TEXT("Apply a safe landscape hole-punch fallback by toggling component visibility inside a world-space AOI."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("landscape_name"), FSololmcpSchemaBuilder::String(TEXT("Actor label of the ALandscape"))},
					{TEXT("region_box"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number(), TEXT("[x_min,y_min,x_max,y_max] in cm"))},
					{TEXT("visible"), FSololmcpSchemaBuilder::Boolean(TEXT("Optional; false punches/hides, true restores/shows"))},
				},
				{TEXT("landscape_name"), TEXT("region_box")}),
			&RunLandscapeHolePunch
		});

		Registry.Register({
			TEXT("swarm_virtual_detachment_mock"),
			TEXT("Mock: isolate a member from a flocking Swarm AI and report the spawn identity. Purely diagnostic."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("swarm_node_id"), FSololmcpSchemaBuilder::Integer(TEXT("Swarm node id"))},
					{TEXT("member_index"),  FSololmcpSchemaBuilder::Integer(TEXT("Member index in the swarm"))},
				},
				{TEXT("swarm_node_id"), TEXT("member_index")}),
			&RunSwarmVirtualDetachmentMock
		});
	}
}
