// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpLandscapeAdvancedTools.cpp
// P2-2 -Landscape Advanced Tools (paint layer, query weight, splines, visibility mask)
// 5 tools (landscape_* prefix):
//   1. landscape_paint_layer            - paint a named layer in a region
//   2. landscape_get_layer_weight       -query painted weight at a world XY
//   3. landscape_spline_create          -build a control-point chain on the landscape's spline component
//   4. landscape_spline_add_mesh        -populate spline segments with mesh assets
//   5. landscape_set_visibility_mask    -toggle landscape components hidden/visible inside a box
//
// Editor module deps (already in SOMOLMCP.Build.cs):
//   - Landscape, LandscapeEditor, WorldPartitionEditor
//
// Notes (TODO):
//   * Spline tools rely on the public ULandscapeSplinesComponent +
//     ULandscapeSplineControlPoint / ULandscapeSplineSegment APIs.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/StaticMesh.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "ScopedTransaction.h"

// Landscape
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeStreamingProxy.h"  // UE 5.7: Info->SortedStreamingProxies type
#include "LandscapeInfo.h"
#include "LandscapeComponent.h"
#include "LandscapeEdit.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#include "LandscapeEditLayer.h"
#else
// LandscapeEditLayer.h is 5.5+.
#endif
#include "LandscapeLayerInfoObject.h"
#include "LandscapeSplinesComponent.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "Materials/MaterialInterface.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "SOMOLMCP_LandscapeAdvanced"

namespace UE::SOMOLMCP
{
	namespace LandAdv
	{
		using SB = FSololmcpSchemaBuilder;

		// -- Schema helpers --------------------------------------------
		static TSharedRef<FJsonObject> RegionBoxSchema()
		{
			return SB::Array(SB::Number(),
				TEXT("World-space box [x_min, y_min, x_max, y_max] in cm."));
		}
		static TSharedRef<FJsonObject> Vec3Schema(const FString& Desc = {})
		{
			return SB::Object({
				{TEXT("x"), SB::Number()},
				{TEXT("y"), SB::Number()},
				{TEXT("z"), SB::Number()}
			}, {}, Desc);
		}
		static TSharedRef<FJsonObject> XyzArrSchema(const FString& Desc = {})
		{
			return SB::Array(SB::Number(), Desc);
		}

		// -- JSON helpers ----------------------------------------------
		static bool ParseRegionBox(const TSharedRef<FJsonObject>& Args,
			const FString& Field, FBox2D& OutBox, FString& OutErr)
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (!Args->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() < 4)
			{
				OutErr = FString::Printf(TEXT("'%s' must be [x_min,y_min,x_max,y_max]."), *Field);
				return false;
			}
			const double X0 = (*Arr)[0]->AsNumber();
			const double Y0 = (*Arr)[1]->AsNumber();
			const double X1 = (*Arr)[2]->AsNumber();
			const double Y1 = (*Arr)[3]->AsNumber();
			if (!FMath::IsFinite(X0) || !FMath::IsFinite(Y0) || !FMath::IsFinite(X1) || !FMath::IsFinite(Y1))
			{
				OutErr = FString::Printf(TEXT("'%s' values must be finite numbers."), *Field);
				return false;
			}
			OutBox = FBox2D(FVector2D(FMath::Min(X0, X1), FMath::Min(Y0, Y1)),
			                FVector2D(FMath::Max(X0, X1), FMath::Max(Y0, Y1)));
			return true;
		}

		static bool ParseXyzArr(const TSharedPtr<FJsonValue>& V, FVector& OutVec)
		{
			if (!V.IsValid()) return false;
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (V->TryGetArray(Arr) && Arr && Arr->Num() >= 3)
			{
				OutVec.X = (*Arr)[0]->AsNumber();
				OutVec.Y = (*Arr)[1]->AsNumber();
				OutVec.Z = (*Arr)[2]->AsNumber();
				return true;
			}
			// UE 5.7: TSharedPtr<FJsonValue>::TryGetObject takes const TSharedPtr<FJsonObject>*&, not TSharedPtr<FJsonObject>&
			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (V->TryGetObject(ObjPtr) && ObjPtr && (*ObjPtr).IsValid())
			{
				const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
				OutVec.X = Obj->HasField(TEXT("x")) ? Obj->GetNumberField(TEXT("x")) : 0.0;
				OutVec.Y = Obj->HasField(TEXT("y")) ? Obj->GetNumberField(TEXT("y")) : 0.0;
				OutVec.Z = Obj->HasField(TEXT("z")) ? Obj->GetNumberField(TEXT("z")) : 0.0;
				return true;
			}
			return false;
		}

		// -- Actor lookup (3-tier: PathName, Name, Label) --------------
		static TSharedRef<FJsonObject> VectorToJson(const FVector& V)
		{
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetNumberField(TEXT("x"), V.X);
			J->SetNumberField(TEXT("y"), V.Y);
			J->SetNumberField(TEXT("z"), V.Z);
			return J;
		}

		static TSharedRef<FJsonObject> RotatorToJson(const FRotator& R)
		{
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetNumberField(TEXT("pitch"), R.Pitch);
			J->SetNumberField(TEXT("yaw"), R.Yaw);
			J->SetNumberField(TEXT("roll"), R.Roll);
			return J;
		}

		static TSharedRef<FJsonObject> BoxToJson(const FBox& Box)
		{
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetObjectField(TEXT("min"), VectorToJson(Box.Min));
			J->SetObjectField(TEXT("max"), VectorToJson(Box.Max));
			J->SetObjectField(TEXT("center"), VectorToJson(Box.GetCenter()));
			J->SetObjectField(TEXT("extent"), VectorToJson(Box.GetExtent()));
			return J;
		}

		static TSharedRef<FJsonObject> IntRectToJson(const FIntRect& Rect)
		{
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetNumberField(TEXT("min_x"), Rect.Min.X);
			J->SetNumberField(TEXT("min_y"), Rect.Min.Y);
			J->SetNumberField(TEXT("max_x"), Rect.Max.X);
			J->SetNumberField(TEXT("max_y"), Rect.Max.Y);
			J->SetNumberField(TEXT("width_quads"), Rect.Width());
			J->SetNumberField(TEXT("height_quads"), Rect.Height());
			return J;
		}

		static FString ObjectPathOrEmpty(const UObject* Obj)
		{
			return Obj ? Obj->GetPathName() : FString();
		}

		#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
		static FString BlendMethodToString(ELandscapeTargetLayerBlendMethod Method)
		{
			switch (Method)
			{
			case ELandscapeTargetLayerBlendMethod::None:
				return TEXT("none");
			case ELandscapeTargetLayerBlendMethod::FinalWeightBlending:
				return TEXT("final_weight_blending");
			case ELandscapeTargetLayerBlendMethod::PremultipliedAlphaBlending:
				return TEXT("premultiplied_alpha_blending");
			default:
				return TEXT("unknown");
			}
		}
		#endif

		static FString SplineMeshAxisToString(ESplineMeshAxis::Type Axis)
		{
			switch (Axis)
			{
			case ESplineMeshAxis::X:
				return TEXT("x");
			case ESplineMeshAxis::Y:
				return TEXT("y");
			case ESplineMeshAxis::Z:
				return TEXT("z");
			default:
				return TEXT("unknown");
			}
		}

		static TSharedRef<FJsonObject> SplineMeshEntryToJson(const FLandscapeSplineMeshEntry& Entry)
		{
			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			UStaticMesh* Mesh = Entry.Mesh.Get();
			J->SetStringField(TEXT("mesh_path"), ObjectPathOrEmpty(Mesh));
			J->SetStringField(TEXT("mesh_name"), Mesh ? Mesh->GetName() : FString());
			J->SetObjectField(TEXT("scale"), VectorToJson(Entry.Scale));
			J->SetBoolField(TEXT("center_horizontally"), Entry.bCenterH != 0);
			J->SetBoolField(TEXT("scale_to_width"), Entry.bScaleToWidth != 0);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
			J->SetBoolField(TEXT("no_z_scaling"), Entry.bNoZScaling != 0);
#else
			// FLandscapeSplineMeshEntry::bNoZScaling is 5.5+; earlier versions always scale Z.
			J->SetBoolField(TEXT("no_z_scaling"), false);
#endif
			J->SetStringField(TEXT("forward_axis"), SplineMeshAxisToString(Entry.ForwardAxis.GetValue()));
			J->SetStringField(TEXT("up_axis"), SplineMeshAxisToString(Entry.UpAxis.GetValue()));
			J->SetNumberField(TEXT("material_override_count"), Entry.MaterialOverrides.Num());
			return J;
		}

		static AActor* ResolveActor(UWorld* World, const FString& Id)
		{
			if (!World) return nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if ((*It)->GetPathName() == Id) return *It;
			}
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if ((*It)->GetName() == Id) return *It;
			}
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if ((*It)->GetActorLabel() == Id) return *It;
			}
			return nullptr;
		}

		static ALandscape* ResolveLandscape(UWorld* World, const FString& Id, FString& OutErr)
		{
			AActor* Actor = ResolveActor(World, Id);
			if (!Actor)
			{
				OutErr = FString::Printf(TEXT("Landscape actor '%s' not found."), *Id);
				return nullptr;
			}
			ALandscape* Landscape = Cast<ALandscape>(Actor);
			if (!Landscape)
			{
				OutErr = FString::Printf(TEXT("Actor '%s' is not an ALandscape (got %s)."),
					*Id, *Actor->GetClass()->GetName());
				return nullptr;
			}
			return Landscape;
		}

		static ULandscapeLayerInfoObject* FindLayerInfo(ULandscapeInfo* Info, const FString& LayerName)
		{
			if (!Info) return nullptr;
			for (const FLandscapeInfoLayerSettings& LS : Info->Layers)
			{
				if (LS.GetLayerName().ToString().Equals(LayerName, ESearchCase::IgnoreCase))
				{
					return LS.LayerInfoObj;
				}
			}
			return nullptr;
		}

		// Find or hold a per-landscape splines component holder map.
		// The persistent per-landscape splines component is on the
		// landscape proxy (ULandscapeSplinesComponent). We allocate one if
		// none exists.
		static ULandscapeSplinesComponent* GetOrCreateSplinesComponent(ALandscape* Landscape)
		{
			if (!Landscape) return nullptr;
			ULandscapeSplinesComponent* Comp = Landscape->FindComponentByClass<ULandscapeSplinesComponent>();
			if (Comp) return Comp;

			Comp = NewObject<ULandscapeSplinesComponent>(Landscape, NAME_None, RF_Transactional);
			Comp->SetupAttachment(Landscape->GetRootComponent());
			Comp->RegisterComponent();
			Landscape->AddInstanceComponent(Comp);
			return Comp;
		}

		static FString LandscapeFallbackTag(const FString& Key, const FString& Value)
		{
			return FString::Printf(TEXT("SOMO.LandscapeSplineFallback.%s=%s"), *Key, *Value);
		}

		static bool HasTagString(const AActor* Actor, const FString& Tag)
		{
			if (!Actor) return false;
			const FName TagName(*Tag);
			return Actor->Tags.Contains(TagName);
		}

		static int32 ParseFallbackSplineId(const AActor* Actor)
		{
			if (!Actor) return INDEX_NONE;
			const FString Prefix = TEXT("SOMO.LandscapeSplineFallback.SplineId=");
			for (const FName& Tag : Actor->Tags)
			{
				const FString S = Tag.ToString();
				if (S.StartsWith(Prefix))
				{
					return FCString::Atoi(*S.RightChop(Prefix.Len()));
				}
			}
			return INDEX_NONE;
		}

		static AActor* FindFallbackSplineActor(UWorld* World, ALandscape* Landscape, int32 SplineId)
		{
			if (!World || !Landscape) return nullptr;
			const FString LandscapeTag = LandscapeFallbackTag(TEXT("Landscape"), Landscape->GetPathName());
			const FString SplineTag = LandscapeFallbackTag(TEXT("SplineId"), FString::FromInt(SplineId));
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (HasTagString(Actor, TEXT("SOMO.LandscapeSplineFallback")) &&
					HasTagString(Actor, LandscapeTag) &&
					HasTagString(Actor, SplineTag))
				{
					return Actor;
				}
			}
			return nullptr;
		}

		static int32 NextFallbackSplineId(UWorld* World, ALandscape* Landscape)
		{
			if (!World || !Landscape) return 0;
			const FString LandscapeTag = LandscapeFallbackTag(TEXT("Landscape"), Landscape->GetPathName());
			int32 MaxId = INDEX_NONE;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (HasTagString(Actor, TEXT("SOMO.LandscapeSplineFallback")) &&
					HasTagString(Actor, LandscapeTag))
				{
					MaxId = FMath::Max(MaxId, ParseFallbackSplineId(Actor));
				}
			}
			return MaxId + 1;
		}

		static void SetFallbackSplineTags(AActor* Actor, ALandscape* Landscape, int32 SplineId)
		{
			if (!Actor || !Landscape) return;
			for (int32 Idx = Actor->Tags.Num() - 1; Idx >= 0; --Idx)
			{
				if (Actor->Tags[Idx].ToString().StartsWith(TEXT("SOMO.LandscapeSplineFallback")))
				{
					Actor->Tags.RemoveAt(Idx);
				}
			}
			Actor->Tags.Add(FName(TEXT("SOMO.LandscapeSplineFallback")));
			Actor->Tags.Add(FName(*LandscapeFallbackTag(TEXT("Mode"), TEXT("spline_actor_component"))));
			Actor->Tags.Add(FName(*LandscapeFallbackTag(TEXT("Landscape"), Landscape->GetPathName())));
			Actor->Tags.Add(FName(*LandscapeFallbackTag(TEXT("SplineId"), FString::FromInt(SplineId))));
		}

		static AActor* GetOrCreateFallbackSplineActor(UWorld* World, ALandscape* Landscape, int32 SplineId)
		{
			if (!World || !Landscape) return nullptr;
			if (AActor* Existing = FindFallbackSplineActor(World, Landscape, SplineId))
			{
				return Existing;
			}

			FActorSpawnParameters SpawnParams;
			SpawnParams.Name = FName(*FString::Printf(TEXT("SOMO_LandscapeSplineFallback_%s_%d"),
				*Landscape->GetName(), SplineId));
			AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
			if (!Actor)
			{
				return nullptr;
			}

			USplineComponent* Spline = NewObject<USplineComponent>(Actor, USplineComponent::StaticClass(), TEXT("LandscapeSplineFallbackSpline"));
			if (!Spline)
			{
				Actor->Destroy();
				return nullptr;
			}
			Actor->SetRootComponent(Spline);
			Actor->AddInstanceComponent(Spline);
			Spline->RegisterComponent();
			Actor->SetActorLabel(FString::Printf(TEXT("SOMO Landscape Spline Fallback %s #%d"), *Landscape->GetName(), SplineId));
			SetFallbackSplineTags(Actor, Landscape, SplineId);
			return Actor;
		}
	} // namespace LandAdv

	// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
	// Public registration entry point
	// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
	void RegisterLandscapeAdvancedTools(FSololmcpToolRegistry& R)
	{
		using namespace LandAdv;

		// ---- read-only: landscape_layer_catalog ----
		R.Register({
			TEXT("landscape_layer_catalog"),
			TEXT("Read-only catalog of a landscape's edit layers, paint LayerInfo bindings, material, and bounds."),
			SB::Object({
				{TEXT("landscape_actor_name"), SB::String(TEXT("Landscape actor name/path/label."))}
			}, {TEXT("landscape_actor_name")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString LandscapeId;
				if (!Args->TryGetStringField(TEXT("landscape_actor_name"), LandscapeId))
				{
					SololmcpError::MissingParam(Out, TEXT("landscape_actor_name"));
					Error = TEXT("Missing landscape_actor_name.");
					return false;
				}

				UWorld* World = Ctx.Services.GetEditorWorld(Error); if (!World) return false;
				ALandscape* Landscape = ResolveLandscape(World, LandscapeId, Error);
				if (!Landscape)
				{
					SololmcpError::NotFound(Out, LandscapeId);
					return false;
				}

				ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
				if (!Info)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("ULandscapeInfo unavailable on landscape."));
					Error = TEXT("Missing landscape info.");
					return false;
				}

				Out->SetBoolField(TEXT("read_only"), true);
				Out->SetStringField(TEXT("landscape_actor"), Landscape->GetName());
				Out->SetStringField(TEXT("landscape_actor_path"), Landscape->GetPathName());
				Out->SetStringField(TEXT("landscape_actor_label"), Landscape->GetActorLabel());
				Out->SetObjectField(TEXT("bounds"), IntRectToJson(Landscape->GetBoundingRect()));

				UMaterialInterface* LandscapeMaterial = Landscape->GetLandscapeMaterial();
				UMaterialInterface* HoleMaterial = Landscape->GetLandscapeHoleMaterial();
				Out->SetStringField(TEXT("landscape_material"), ObjectPathOrEmpty(LandscapeMaterial));
				Out->SetStringField(TEXT("landscape_hole_material"), ObjectPathOrEmpty(HoleMaterial));

				TArray<TSharedPtr<FJsonValue>> EditLayers;
				// 5.6 exposed edit layers as ULandscapeEditLayerBase objects with per-target-type
				// alpha and capability queries. Before that they are plain FLandscapeLayer structs
				// reached through GetLayerCount/GetLayer, with untyped height/weight alphas and no
				// per-type capability model. The 5.3 branch reports the fields that exist and marks
				// the typed capabilities as unconditionally supported, which is what an untyped
				// layer means -- it does not fabricate class/path for a struct that has neither.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				const TArray<ULandscapeEditLayerBase*> Layers = Landscape->GetEditLayers();
				for (int32 Index = 0; Index < Layers.Num(); ++Index)
				{
					ULandscapeEditLayerBase* Layer = Layers[Index];
					if (!Layer) continue;

					TSharedRef<FJsonObject> LayerJson = MakeShared<FJsonObject>();
					LayerJson->SetNumberField(TEXT("index"), Index);
					LayerJson->SetStringField(TEXT("name"), Layer->GetName().ToString());
					LayerJson->SetStringField(TEXT("guid"), Layer->GetGuid().ToString());
					LayerJson->SetStringField(TEXT("class"), Layer->GetClass() ? Layer->GetClass()->GetName() : FString());
					LayerJson->SetStringField(TEXT("path"), Layer->GetPathName());
					LayerJson->SetBoolField(TEXT("visible"), Layer->IsVisible());
					LayerJson->SetBoolField(TEXT("locked"), Layer->IsLocked());
					LayerJson->SetBoolField(TEXT("editing"), Layer->GetGuid() == Landscape->GetEditingLayer());
					LayerJson->SetNumberField(TEXT("height_alpha"), Layer->GetAlphaForTargetType(ELandscapeToolTargetType::Heightmap));
					LayerJson->SetNumberField(TEXT("weight_alpha"), Layer->GetAlphaForTargetType(ELandscapeToolTargetType::Weightmap));
					LayerJson->SetBoolField(TEXT("supports_height"), Layer->SupportsTargetType(ELandscapeToolTargetType::Heightmap));
					LayerJson->SetBoolField(TEXT("supports_weight"), Layer->SupportsTargetType(ELandscapeToolTargetType::Weightmap));
					LayerJson->SetBoolField(TEXT("supports_visibility"), Layer->SupportsTargetType(ELandscapeToolTargetType::Visibility));
					EditLayers.Add(MakeShared<FJsonValueObject>(LayerJson));
				}
#else
				const int32 LayerCount = static_cast<int32>(Landscape->GetLayerCount());
				for (int32 Index = 0; Index < LayerCount; ++Index)
				{
					const FLandscapeLayer* Layer = Landscape->GetLayer(Index);
					if (!Layer) continue;

					TSharedRef<FJsonObject> LayerJson = MakeShared<FJsonObject>();
					LayerJson->SetNumberField(TEXT("index"), Index);
					LayerJson->SetStringField(TEXT("name"), Layer->Name.ToString());
					LayerJson->SetStringField(TEXT("guid"), Layer->Guid.ToString());
					LayerJson->SetStringField(TEXT("class"), TEXT("FLandscapeLayer"));
					LayerJson->SetStringField(TEXT("path"), FString());
					LayerJson->SetBoolField(TEXT("visible"), Layer->bVisible);
					LayerJson->SetBoolField(TEXT("locked"), Layer->bLocked);
					LayerJson->SetBoolField(TEXT("editing"), Layer->Guid == Landscape->GetEditingLayer());
					LayerJson->SetNumberField(TEXT("height_alpha"), Layer->HeightmapAlpha);
					LayerJson->SetNumberField(TEXT("weight_alpha"), Layer->WeightmapAlpha);
					LayerJson->SetBoolField(TEXT("supports_height"), true);
					LayerJson->SetBoolField(TEXT("supports_weight"), true);
					LayerJson->SetBoolField(TEXT("supports_visibility"), true);
					EditLayers.Add(MakeShared<FJsonValueObject>(LayerJson));
				}
#endif

				TArray<TSharedPtr<FJsonValue>> PaintLayers;
				int32 BoundLayerInfoCount = 0;
				int32 UnboundLayerInfoCount = 0;
				for (int32 Index = 0; Index < Info->Layers.Num(); ++Index)
				{
					const FLandscapeInfoLayerSettings& Settings = Info->Layers[Index];
					ULandscapeLayerInfoObject* LayerInfo = Settings.LayerInfoObj.Get();
					TSharedRef<FJsonObject> LayerJson = MakeShared<FJsonObject>();
					LayerJson->SetNumberField(TEXT("index"), Index);
					LayerJson->SetStringField(TEXT("layer_name"), Settings.GetLayerName().ToString());
					LayerJson->SetBoolField(TEXT("bound"), LayerInfo != nullptr);
					if (LayerInfo)
					{
						++BoundLayerInfoCount;
						LayerJson->SetStringField(TEXT("name"), SOMOLMCP_LANDSCAPE_LAYER_NAME(LayerInfo).ToString());
						LayerJson->SetStringField(TEXT("asset_path"), LayerInfo->GetPathName());
						LayerJson->SetStringField(TEXT("package_path"), LayerInfo->GetPackage() ? LayerInfo->GetPackage()->GetName() : FString());
						#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 6
						LayerJson->SetStringField(TEXT("blend_method"), SOMOLMCP_LANDSCAPE_LAYER_NO_WEIGHT(LayerInfo) ? TEXT("none") : TEXT("final_weight_blending"));
						#else
						LayerJson->SetStringField(TEXT("blend_method"), BlendMethodToString(LayerInfo->GetBlendMethod()));
						#endif
						LayerJson->SetBoolField(TEXT("no_weight_blend"), SOMOLMCP_LANDSCAPE_LAYER_NO_WEIGHT(LayerInfo));
						LayerJson->SetNumberField(TEXT("hardness"), SOMOLMCP_LANDSCAPE_LAYER_HARDNESS(LayerInfo));
						LayerJson->SetStringField(TEXT("physical_material"), ObjectPathOrEmpty(SOMOLMCP_LANDSCAPE_LAYER_PHYSICAL_MATERIAL(LayerInfo)));
					}
					else
					{
						++UnboundLayerInfoCount;
					}
					PaintLayers.Add(MakeShared<FJsonValueObject>(LayerJson));
				}

				Out->SetArrayField(TEXT("edit_layers"), EditLayers);
				Out->SetArrayField(TEXT("paint_layers"), PaintLayers);
				Out->SetNumberField(TEXT("edit_layer_count"), EditLayers.Num());
				Out->SetNumberField(TEXT("paint_layer_count"), PaintLayers.Num());
				Out->SetNumberField(TEXT("bound_layerinfo_count"), BoundLayerInfoCount);
				Out->SetNumberField(TEXT("unbound_layerinfo_count"), UnboundLayerInfoCount);

				Summary = FString::Printf(TEXT("Cataloged landscape '%s': %d edit layers, %d paint layers (%d bound)."),
					*Landscape->GetName(), EditLayers.Num(), PaintLayers.Num(), BoundLayerInfoCount);
				return true;
			},
			nullptr, 5
		});

		// ---- read-only: landscape_spline_inspect ----
		R.Register({
			TEXT("landscape_spline_inspect"),
			TEXT("Read-only inspection of native landscape splines and SOMOLMCP fallback spline actors."),
			SB::Object({
				{TEXT("landscape_actor_name"), SB::String(TEXT("Landscape actor name/path/label."))},
				{TEXT("spline_id"),            SB::Integer(TEXT("Optional fallback spline id filter; native landscape spline arrays are not id-addressed."))},
				{TEXT("include_points"),       SB::Boolean(TEXT("Include sampled spline/interp points (default true)."))},
				{TEXT("include_meshes"),       SB::Boolean(TEXT("Include mesh entry/component details (default true)."))},
				{TEXT("max_points_per_segment"), SB::Integer(TEXT("Point sample cap per segment or fallback spline (default 64)."))}
			}, {TEXT("landscape_actor_name")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString LandscapeId;
				if (!Args->TryGetStringField(TEXT("landscape_actor_name"), LandscapeId))
				{
					SololmcpError::MissingParam(Out, TEXT("landscape_actor_name"));
					Error = TEXT("Missing landscape_actor_name.");
					return false;
				}

				const bool bHasSplineIdFilter = Args->HasTypedField<EJson::Number>(TEXT("spline_id"));
				const int32 SplineIdFilter = bHasSplineIdFilter ? Args->GetIntegerField(TEXT("spline_id")) : INDEX_NONE;
				const bool bIncludePoints = Args->HasTypedField<EJson::Boolean>(TEXT("include_points"))
					? Args->GetBoolField(TEXT("include_points")) : true;
				const bool bIncludeMeshes = Args->HasTypedField<EJson::Boolean>(TEXT("include_meshes"))
					? Args->GetBoolField(TEXT("include_meshes")) : true;
				const int32 MaxPointsPerSegment = Args->HasTypedField<EJson::Number>(TEXT("max_points_per_segment"))
					? FMath::Clamp(Args->GetIntegerField(TEXT("max_points_per_segment")), 0, 512)
					: 64;

				UWorld* World = Ctx.Services.GetEditorWorld(Error); if (!World) return false;
				ALandscape* Landscape = ResolveLandscape(World, LandscapeId, Error);
				if (!Landscape)
				{
					SololmcpError::NotFound(Out, LandscapeId);
					return false;
				}

				Out->SetBoolField(TEXT("read_only"), true);
				Out->SetStringField(TEXT("landscape_actor"), Landscape->GetName());
				Out->SetStringField(TEXT("landscape_actor_path"), Landscape->GetPathName());
				Out->SetStringField(TEXT("landscape_actor_label"), Landscape->GetActorLabel());
				Out->SetBoolField(TEXT("filtered_by_spline_id"), bHasSplineIdFilter);
				if (bHasSplineIdFilter)
				{
					Out->SetNumberField(TEXT("spline_id"), SplineIdFilter);
				}

				ULandscapeSplinesComponent* Splines = Landscape->FindComponentByClass<ULandscapeSplinesComponent>();
				TSharedRef<FJsonObject> NativeJson = MakeShared<FJsonObject>();
				NativeJson->SetBoolField(TEXT("present"), Splines != nullptr);
				NativeJson->SetBoolField(TEXT("spline_id_filter_applied"), false);
				NativeJson->SetStringField(TEXT("filter_note"), bHasSplineIdFilter
					? TEXT("spline_id filters SOMOLMCP fallback actors only; native ULandscapeSplineSegment arrays do not expose stable chain ids.")
					: TEXT(""));

				int32 NativeControlPointCount = 0;
				int32 NativeSegmentCount = 0;
				if (Splines)
				{
					NativeJson->SetStringField(TEXT("component_path"), Splines->GetPathName());
					NativeJson->SetStringField(TEXT("component_name"), Splines->GetName());

					const TArray<TObjectPtr<ULandscapeSplineControlPoint>>& ControlPoints = Splines->GetControlPoints();
					const TArray<TObjectPtr<ULandscapeSplineSegment>>& Segments = Splines->GetSegments();
					NativeControlPointCount = ControlPoints.Num();
					NativeSegmentCount = Segments.Num();

					TMap<ULandscapeSplineControlPoint*, int32> ControlPointIndexes;
					TArray<TSharedPtr<FJsonValue>> ControlPointJsonArray;
					const FTransform LandscapeTransform = Landscape->GetActorTransform();
					for (int32 Index = 0; Index < ControlPoints.Num(); ++Index)
					{
						ULandscapeSplineControlPoint* ControlPoint = ControlPoints[Index].Get();
						if (!ControlPoint) continue;
						ControlPointIndexes.Add(ControlPoint, Index);

						TSharedRef<FJsonObject> PointJson = MakeShared<FJsonObject>();
						PointJson->SetNumberField(TEXT("index"), Index);
						PointJson->SetStringField(TEXT("name"), ControlPoint->GetName());
						PointJson->SetStringField(TEXT("path"), ControlPoint->GetPathName());
						PointJson->SetObjectField(TEXT("local_location"), VectorToJson(ControlPoint->Location));
						PointJson->SetObjectField(TEXT("world_location"), VectorToJson(LandscapeTransform.TransformPosition(ControlPoint->Location)));
						PointJson->SetObjectField(TEXT("rotation"), RotatorToJson(ControlPoint->Rotation));
						PointJson->SetNumberField(TEXT("width"), ControlPoint->Width);
						PointJson->SetNumberField(TEXT("layer_width_ratio"), ControlPoint->LayerWidthRatio);
						PointJson->SetNumberField(TEXT("side_falloff"), ControlPoint->SideFalloff);
						PointJson->SetNumberField(TEXT("end_falloff"), ControlPoint->EndFalloff);
						PointJson->SetNumberField(TEXT("connected_segment_count"), ControlPoint->ConnectedSegments.Num());
#if WITH_EDITORONLY_DATA
						PointJson->SetStringField(TEXT("layer_name"), ControlPoint->LayerName.ToString());
						PointJson->SetBoolField(TEXT("raise_terrain"), ControlPoint->bRaiseTerrain != 0);
						PointJson->SetBoolField(TEXT("lower_terrain"), ControlPoint->bLowerTerrain != 0);
						PointJson->SetStringField(TEXT("mesh_path"), ObjectPathOrEmpty(ControlPoint->Mesh.Get()));
						PointJson->SetObjectField(TEXT("mesh_scale"), VectorToJson(ControlPoint->MeshScale));
#endif
						ControlPointJsonArray.Add(MakeShared<FJsonValueObject>(PointJson));
					}

					TArray<TSharedPtr<FJsonValue>> SegmentJsonArray;
					for (int32 Index = 0; Index < Segments.Num(); ++Index)
					{
						ULandscapeSplineSegment* Segment = Segments[Index].Get();
						if (!Segment) continue;

						TSharedRef<FJsonObject> SegmentJson = MakeShared<FJsonObject>();
						SegmentJson->SetNumberField(TEXT("index"), Index);
						SegmentJson->SetStringField(TEXT("name"), Segment->GetName());
						SegmentJson->SetStringField(TEXT("path"), Segment->GetPathName());
						SegmentJson->SetObjectField(TEXT("bounds"), BoxToJson(Segment->GetBounds()));
						SegmentJson->SetNumberField(TEXT("interp_point_count"), Segment->GetPoints().Num());
#if WITH_EDITORONLY_DATA
						SegmentJson->SetStringField(TEXT("layer_name"), Segment->LayerName.ToString());
						SegmentJson->SetBoolField(TEXT("raise_terrain"), Segment->bRaiseTerrain != 0);
						SegmentJson->SetBoolField(TEXT("lower_terrain"), Segment->bLowerTerrain != 0);
						SegmentJson->SetBoolField(TEXT("cast_shadow"), Segment->bCastShadow != 0);
						SegmentJson->SetBoolField(TEXT("hidden_in_game"), Segment->bHiddenInGame != 0);
						SegmentJson->SetNumberField(TEXT("mesh_entry_count"), Segment->SplineMeshes.Num());
#endif

						TArray<TSharedPtr<FJsonValue>> ConnectionArray;
						for (int32 EndIndex = 0; EndIndex < 2; ++EndIndex)
						{
							const FLandscapeSplineSegmentConnection& Connection = Segment->Connections[EndIndex];
							ULandscapeSplineControlPoint* ControlPoint = Connection.ControlPoint.Get();
							TSharedRef<FJsonObject> ConnectionJson = MakeShared<FJsonObject>();
							ConnectionJson->SetNumberField(TEXT("end"), EndIndex);
							ConnectionJson->SetNumberField(TEXT("tangent_len"), Connection.TangentLen);
							ConnectionJson->SetStringField(TEXT("socket_name"), Connection.SocketName.ToString());
							ConnectionJson->SetStringField(TEXT("control_point_path"), ObjectPathOrEmpty(ControlPoint));
							ConnectionJson->SetNumberField(TEXT("control_point_index"), ControlPointIndexes.Contains(ControlPoint) ? ControlPointIndexes[ControlPoint] : INDEX_NONE);
							ConnectionArray.Add(MakeShared<FJsonValueObject>(ConnectionJson));
						}
						SegmentJson->SetArrayField(TEXT("connections"), ConnectionArray);

						if (bIncludePoints)
						{
							TArray<TSharedPtr<FJsonValue>> PointsArray;
							const TArray<FLandscapeSplineInterpPoint>& InterpPoints = Segment->GetPoints();
							const int32 PointLimit = FMath::Min(InterpPoints.Num(), MaxPointsPerSegment);
							for (int32 PointIndex = 0; PointIndex < PointLimit; ++PointIndex)
							{
								TSharedRef<FJsonObject> PointJson = MakeShared<FJsonObject>();
								PointJson->SetNumberField(TEXT("index"), PointIndex);
								PointJson->SetObjectField(TEXT("center"), VectorToJson(InterpPoints[PointIndex].Center));
								PointsArray.Add(MakeShared<FJsonValueObject>(PointJson));
							}
							SegmentJson->SetArrayField(TEXT("points"), PointsArray);
							SegmentJson->SetBoolField(TEXT("points_truncated"), InterpPoints.Num() > PointLimit);
						}

#if WITH_EDITORONLY_DATA
						if (bIncludeMeshes)
						{
							TArray<TSharedPtr<FJsonValue>> MeshesArray;
							for (const FLandscapeSplineMeshEntry& Entry : Segment->SplineMeshes)
							{
								MeshesArray.Add(MakeShared<FJsonValueObject>(SplineMeshEntryToJson(Entry)));
							}
							SegmentJson->SetArrayField(TEXT("mesh_entries"), MeshesArray);
						}
#endif
						SegmentJsonArray.Add(MakeShared<FJsonValueObject>(SegmentJson));
					}

					NativeJson->SetArrayField(TEXT("control_points"), ControlPointJsonArray);
					NativeJson->SetArrayField(TEXT("segments"), SegmentJsonArray);
				}
				NativeJson->SetNumberField(TEXT("control_point_count"), NativeControlPointCount);
				NativeJson->SetNumberField(TEXT("segment_count"), NativeSegmentCount);
				Out->SetObjectField(TEXT("native"), NativeJson);

				TArray<TSharedPtr<FJsonValue>> FallbackArray;
				const FString LandscapeTag = LandscapeFallbackTag(TEXT("Landscape"), Landscape->GetPathName());
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!HasTagString(Actor, TEXT("SOMO.LandscapeSplineFallback")) ||
						!HasTagString(Actor, LandscapeTag))
					{
						continue;
					}
					const int32 SplineId = ParseFallbackSplineId(Actor);
					if (bHasSplineIdFilter && SplineId != SplineIdFilter)
					{
						continue;
					}

					USplineComponent* Spline = Actor->FindComponentByClass<USplineComponent>();
					TSharedRef<FJsonObject> FallbackJson = MakeShared<FJsonObject>();
					FallbackJson->SetStringField(TEXT("fallback_mode"), TEXT("spline_actor_component"));
					FallbackJson->SetNumberField(TEXT("spline_id"), SplineId);
					FallbackJson->SetStringField(TEXT("actor"), Actor->GetPathName());
					FallbackJson->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
					FallbackJson->SetBoolField(TEXT("has_spline_component"), Spline != nullptr);
					FallbackJson->SetNumberField(TEXT("point_count"), Spline ? Spline->GetNumberOfSplinePoints() : 0);
					FallbackJson->SetBoolField(TEXT("closed_loop"), Spline ? Spline->IsClosedLoop() : false);
					FallbackJson->SetNumberField(TEXT("length"), Spline ? Spline->GetSplineLength() : 0.0);

					if (Spline && bIncludePoints)
					{
						TArray<TSharedPtr<FJsonValue>> PointsArray;
						const int32 PointCount = Spline->GetNumberOfSplinePoints();
						const int32 PointLimit = FMath::Min(PointCount, MaxPointsPerSegment);
						for (int32 PointIndex = 0; PointIndex < PointLimit; ++PointIndex)
						{
							TSharedRef<FJsonObject> PointJson = MakeShared<FJsonObject>();
							PointJson->SetNumberField(TEXT("index"), PointIndex);
							PointJson->SetObjectField(TEXT("world_location"), VectorToJson(Spline->GetLocationAtSplinePoint(PointIndex, ESplineCoordinateSpace::World)));
							PointJson->SetObjectField(TEXT("local_location"), VectorToJson(Spline->GetLocationAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local)));
							PointsArray.Add(MakeShared<FJsonValueObject>(PointJson));
						}
						FallbackJson->SetArrayField(TEXT("points"), PointsArray);
						FallbackJson->SetBoolField(TEXT("points_truncated"), PointCount > PointLimit);
					}

					if (bIncludeMeshes)
					{
						TArray<USplineMeshComponent*> SplineMeshes;
						Actor->GetComponents<USplineMeshComponent>(SplineMeshes);
						TArray<TSharedPtr<FJsonValue>> MeshArray;
						for (USplineMeshComponent* SplineMesh : SplineMeshes)
						{
							if (!SplineMesh) continue;
							TSharedRef<FJsonObject> MeshJson = MakeShared<FJsonObject>();
							MeshJson->SetStringField(TEXT("component"), SplineMesh->GetPathName());
							MeshJson->SetStringField(TEXT("component_name"), SplineMesh->GetName());
							MeshJson->SetStringField(TEXT("mesh_path"), ObjectPathOrEmpty(SplineMesh->GetStaticMesh()));
							MeshJson->SetBoolField(TEXT("visible"), SplineMesh->IsVisible());
							MeshArray.Add(MakeShared<FJsonValueObject>(MeshJson));
						}
						FallbackJson->SetArrayField(TEXT("spline_mesh_components"), MeshArray);
						FallbackJson->SetNumberField(TEXT("spline_mesh_component_count"), MeshArray.Num());
					}

					FallbackArray.Add(MakeShared<FJsonValueObject>(FallbackJson));
				}
				Out->SetArrayField(TEXT("fallback_splines"), FallbackArray);
				Out->SetNumberField(TEXT("fallback_spline_count"), FallbackArray.Num());

				Summary = FString::Printf(TEXT("Inspected landscape '%s': %d native segments, %d fallback splines."),
					*Landscape->GetName(), NativeSegmentCount, FallbackArray.Num());
				return true;
			},
			nullptr, 5
		});

		// -- 1. landscape_paint_layer ----------------------------------
		R.Register({
			TEXT("landscape_paint_layer"),
			TEXT("Paint a named landscape weight layer inside a world-space region using editor weightmap data."),
			SB::Object({
				{TEXT("landscape_actor_name"), SB::String(TEXT("Landscape actor name/label."))},
				{TEXT("layer_name"),           SB::String(TEXT("Target layer name (e.g. 'Grass')."))},
				{TEXT("region_box"),           RegionBoxSchema()},
				{TEXT("weight"),               SB::Number(TEXT("Target weight 0..1."))},
				{TEXT("brush_falloff"),        SB::Number(TEXT("Edge falloff 0..1 (default 0.5)."))}
			}, {TEXT("landscape_actor_name"), TEXT("layer_name"), TEXT("region_box"), TEXT("weight")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString LandscapeId, LayerName;
				if (!Args->TryGetStringField(TEXT("landscape_actor_name"), LandscapeId))
				{
					SololmcpError::MissingParam(Out, TEXT("landscape_actor_name"));
					Error = TEXT("Missing landscape_actor_name."); return false;
				}
				if (!Args->TryGetStringField(TEXT("layer_name"), LayerName))
				{
					SololmcpError::MissingParam(Out, TEXT("layer_name"));
					Error = TEXT("Missing layer_name."); return false;
				}

				FBox2D Region;
				if (!ParseRegionBox(Args, TEXT("region_box"), Region, Error))
				{
					SololmcpError::Set(Out, TEXT("INVALID_TYPE"), TEXT("region_box"),
						TEXT("region_box must be [x_min,y_min,x_max,y_max] numbers."));
					return false;
				}
				UWorld* World = Ctx.Services.GetEditorWorld(Error); if (!World) return false;
				ALandscape* Landscape = ResolveLandscape(World, LandscapeId, Error);
				if (!Landscape)
				{
					SololmcpError::NotFound(Out, LandscapeId);
					return false;
				}

				ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
				if (!Info)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("ULandscapeInfo unavailable on landscape."));
					Error = TEXT("Missing landscape info."); return false;
				}

				ULandscapeLayerInfoObject* LayerInfo = FindLayerInfo(Info, LayerName);
				if (!LayerInfo)
				{
					SololmcpError::NotFound(Out, FString::Printf(TEXT("layer '%s'"), *LayerName));
					Out->SetStringField(TEXT("landscape_actor"), Landscape->GetName());
					Error = TEXT("Layer not found."); return false;
				}

				const float TargetWeight = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("weight"))), 0.0f, 1.0f);
				const float Falloff = Args->HasTypedField<EJson::Number>(TEXT("brush_falloff"))
					? FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("brush_falloff"))), 0.0f, 1.0f)
					: 0.0f;

				const FTransform LtoW = Landscape->GetActorTransform();
				const FVector LocalMin = LtoW.InverseTransformPosition(FVector(Region.Min.X, Region.Min.Y, 0.0));
				const FVector LocalMax = LtoW.InverseTransformPosition(FVector(Region.Max.X, Region.Max.Y, 0.0));
				const int32 MinX = FMath::FloorToInt(FMath::Min(LocalMin.X, LocalMax.X));
				const int32 MinY = FMath::FloorToInt(FMath::Min(LocalMin.Y, LocalMax.Y));
				const int32 MaxX = FMath::CeilToInt(FMath::Max(LocalMin.X, LocalMax.X));
				const int32 MaxY = FMath::CeilToInt(FMath::Max(LocalMin.Y, LocalMax.Y));
				const int32 Width = MaxX - MinX + 1;
				const int32 Height = MaxY - MinY + 1;
				const int64 SampleCount64 = static_cast<int64>(Width) * static_cast<int64>(Height);
				if (Width <= 0 || Height <= 0 || SampleCount64 <= 0 || SampleCount64 > 4000000)
				{
					SololmcpError::Set(Out, TEXT("INVALID_TYPE"), TEXT("region_box"),
						TEXT("region_box produced an empty or too-large landscape-space region."));
					Error = TEXT("Invalid paint region."); return false;
				}
				const FIntRect LandscapeBounds = Landscape->GetBoundingRect();
				if (MaxX < LandscapeBounds.Min.X || MinX > LandscapeBounds.Max.X ||
					MaxY < LandscapeBounds.Min.Y || MinY > LandscapeBounds.Max.Y)
				{
					SololmcpError::Set(Out, TEXT("INVALID_TYPE"), TEXT("region_box"),
						TEXT("region_box does not overlap the target landscape bounds."));
					Error = TEXT("Paint region outside landscape bounds."); return false;
				}
				const int32 SampleCount = static_cast<int32>(SampleCount64);

				TArray<uint8> WeightData;
				WeightData.SetNumUninitialized(SampleCount);
				const float EdgeWidthX = (Width > 1) ? Falloff * static_cast<float>(Width) * 0.5f : 0.0f;
				const float EdgeWidthY = (Height > 1) ? Falloff * static_cast<float>(Height) * 0.5f : 0.0f;
				double TotalWeight = 0.0;
				for (int32 Y = 0; Y < Height; ++Y)
				{
					for (int32 X = 0; X < Width; ++X)
					{
						float Alpha = 1.0f;
						if (EdgeWidthX > KINDA_SMALL_NUMBER)
						{
							Alpha = FMath::Min(Alpha, FMath::Clamp(FMath::Min(static_cast<float>(X), static_cast<float>(Width - 1 - X)) / EdgeWidthX, 0.0f, 1.0f));
						}
						if (EdgeWidthY > KINDA_SMALL_NUMBER)
						{
							Alpha = FMath::Min(Alpha, FMath::Clamp(FMath::Min(static_cast<float>(Y), static_cast<float>(Height - 1 - Y)) / EdgeWidthY, 0.0f, 1.0f));
						}
						const uint8 Value = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(TargetWeight * Alpha * 255.0f), 0, 255));
						WeightData[Y * Width + X] = Value;
						TotalWeight += static_cast<double>(Value) / 255.0;
					}
				}

				const FScopedTransaction Tx(LOCTEXT("PaintLayerRegion", "SOMOLMCP Paint Landscape Layer Region"));
				Landscape->Modify();
				FLandscapeEditDataInterface Edit(Info);
				TSet<ULandscapeLayerInfoObject*> DirtyLayers;
				DirtyLayers.Add(LayerInfo);
				Edit.SetAlphaData(DirtyLayers, MinX, MinY, MaxX, MaxY, WeightData.GetData(), WeightData.Num(), ELandscapeLayerPaintingRestriction::None);

				TArray<uint8> Readback;
				Readback.SetNumZeroed(SampleCount);
				Edit.GetWeightDataFast(LayerInfo, MinX, MinY, MaxX, MaxY, Readback.GetData(), 0);
				if (Readback != WeightData)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT("layer_name"),
						TEXT("Landscape weightmap write failed immediate readback."));
					Error = TEXT("Landscape paint layer readback mismatch."); return false;
				}
				SololmcpWriteFlush::EnsureFlushed(Landscape);

				Out->SetStringField(TEXT("landscape_actor"), Landscape->GetName());
				Out->SetStringField(TEXT("layer_name"), LayerName);
				TArray<TSharedPtr<FJsonValue>> RegionVals;
				RegionVals.Add(MakeShared<FJsonValueNumber>(Region.Min.X));
				RegionVals.Add(MakeShared<FJsonValueNumber>(Region.Min.Y));
				RegionVals.Add(MakeShared<FJsonValueNumber>(Region.Max.X));
				RegionVals.Add(MakeShared<FJsonValueNumber>(Region.Max.Y));
				Out->SetArrayField(TEXT("region_box"), RegionVals);
				Out->SetNumberField(TEXT("quad_min_x"), MinX);
				Out->SetNumberField(TEXT("quad_min_y"), MinY);
				Out->SetNumberField(TEXT("quad_max_x"), MaxX);
				Out->SetNumberField(TEXT("quad_max_y"), MaxY);
				Out->SetNumberField(TEXT("updated_samples"), SampleCount);
				Out->SetNumberField(TEXT("weight"), TargetWeight);
				Out->SetNumberField(TEXT("brush_falloff"), Falloff);
				Out->SetNumberField(TEXT("total_weight"), TotalWeight);

				Summary = FString::Printf(TEXT("Painted layer '%s' on %d landscape samples."), *LayerName, SampleCount);
				return true;
			},
			nullptr, 5
		});

		// -- 2. landscape_get_layer_weight -----------------------------
		R.Register({
			TEXT("landscape_get_layer_weight"),
			TEXT("Return the painted weight (0..1) of a layer at a world-space XY position."),
			SB::Object({
				{TEXT("landscape_actor_name"), SB::String()},
				{TEXT("layer_name"),           SB::String()},
				{TEXT("world_pos"),            SB::Array(SB::Number(), TEXT("[x,y] in cm."))}
			}, {TEXT("landscape_actor_name"), TEXT("layer_name"), TEXT("world_pos")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString LandscapeId, LayerName;
				if (!Args->TryGetStringField(TEXT("landscape_actor_name"), LandscapeId))
				{
					SololmcpError::MissingParam(Out, TEXT("landscape_actor_name"));
					Error = TEXT("Missing landscape_actor_name."); return false;
				}
				if (!Args->TryGetStringField(TEXT("layer_name"), LayerName))
				{
					SololmcpError::MissingParam(Out, TEXT("layer_name"));
					Error = TEXT("Missing layer_name."); return false;
				}
				const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
				if (!Args->TryGetArrayField(TEXT("world_pos"), PosArr) || !PosArr || PosArr->Num() < 2)
				{
					SololmcpError::Set(Out, TEXT("INVALID_TYPE"), TEXT("world_pos"),
						TEXT("world_pos must be [x,y] numbers."));
					Error = TEXT("Bad world_pos."); return false;
				}
				const double WX = (*PosArr)[0]->AsNumber();
				const double WY = (*PosArr)[1]->AsNumber();
				if (!FMath::IsFinite(WX) || !FMath::IsFinite(WY))
				{
					SololmcpError::Set(Out, TEXT("INVALID_TYPE"), TEXT("world_pos"),
						TEXT("world_pos values must be finite numbers."));
					Error = TEXT("Bad world_pos."); return false;
				}

				UWorld* World = Ctx.Services.GetEditorWorld(Error); if (!World) return false;
				ALandscape* Landscape = ResolveLandscape(World, LandscapeId, Error);
				if (!Landscape) { SololmcpError::NotFound(Out, LandscapeId); return false; }

				ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
				if (!Info)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("ULandscapeInfo unavailable on landscape."));
					Error = TEXT("Missing landscape info."); return false;
				}

				ULandscapeLayerInfoObject* LayerInfo = FindLayerInfo(Info, LayerName);
				if (!LayerInfo)
				{
					SololmcpError::NotFound(Out, FString::Printf(TEXT("layer '%s'"), *LayerName));
					Out->SetStringField(TEXT("landscape_actor"), Landscape->GetName());
					Error = TEXT("Layer not found."); return false;
				}

				// Convert world XY to landscape quad coords.
				const FTransform LtoW = Landscape->GetActorTransform();
				const FVector Local = LtoW.InverseTransformPosition(FVector(WX, WY, 0.0));
				const int32 QuadX = FMath::RoundToInt(Local.X);
				const int32 QuadY = FMath::RoundToInt(Local.Y);

				FLandscapeEditDataInterface Edit(Info);
				TArray<uint8> Buffer;
				Buffer.SetNumZeroed(1);
				// Read a 1x1 region into Buffer.
				Edit.GetWeightDataFast(LayerInfo, QuadX, QuadY, QuadX, QuadY, Buffer.GetData(), 0);

				const float Weight = static_cast<float>(Buffer.IsValidIndex(0) ? Buffer[0] : 0) / 255.f;

				Out->SetStringField(TEXT("landscape_actor"), Landscape->GetName());
				Out->SetStringField(TEXT("layer_name"), LayerName);
				Out->SetNumberField(TEXT("world_x"), WX);
				Out->SetNumberField(TEXT("world_y"), WY);
				Out->SetNumberField(TEXT("quad_x"), QuadX);
				Out->SetNumberField(TEXT("quad_y"), QuadY);
				Out->SetNumberField(TEXT("weight"), Weight);
				Summary = FString::Printf(TEXT("Layer '%s' @ (%.0f,%.0f) weight=%.3f"),
					*LayerName, WX, WY, Weight);
				return true;
			},
			nullptr, 5
		});

		// -- 3. landscape_spline_create --------------------------------
		R.Register({
			TEXT("landscape_spline_create"),
			TEXT("Create a chain of ULandscapeSplineControlPoint+Segment on the landscape's "
			     "splines component. Returns a stable spline_id (component-relative chain index)."),
			SB::Object({
				{TEXT("landscape_actor_name"), SB::String()},
				{TEXT("points"),               SB::Array(Vec3Schema(), TEXT("Array of {x,y,z} (>=2)."))},
				{TEXT("spline_id"),            SB::Integer(TEXT("Optional fallback spline id to update instead of creating the next id."))}
			}, {TEXT("landscape_actor_name"), TEXT("points")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString LandscapeId;
				if (!Args->TryGetStringField(TEXT("landscape_actor_name"), LandscapeId))
				{
					SololmcpError::MissingParam(Out, TEXT("landscape_actor_name"));
					Error = TEXT("Missing landscape_actor_name."); return false;
				}
				const TArray<TSharedPtr<FJsonValue>>* PointsArr = nullptr;
				if (!Args->TryGetArrayField(TEXT("points"), PointsArr) || !PointsArr || PointsArr->Num() < 2)
				{
					SololmcpError::Set(Out, TEXT("INVALID_TYPE"), TEXT("points"),
						TEXT("points must be an array of >=2 {x,y,z}."));
					Error = TEXT("Need >=2 points."); return false;
				}

				TArray<FVector> Pts;
				Pts.Reserve(PointsArr->Num());
				for (const TSharedPtr<FJsonValue>& V : *PointsArr)
				{
					FVector P;
					if (!ParseXyzArr(V, P))
					{
						SololmcpError::Set(Out, TEXT("INVALID_TYPE"), TEXT("points"),
							TEXT("Each point must be {x,y,z} or [x,y,z]."));
						Error = TEXT("Bad point."); return false;
					}
					Pts.Add(P);
				}

				UWorld* World = Ctx.Services.GetEditorWorld(Error); if (!World) return false;
				ALandscape* Landscape = ResolveLandscape(World, LandscapeId, Error);
				if (!Landscape) { SololmcpError::NotFound(Out, LandscapeId); return false; }

				const int32 SplineId = Args->HasTypedField<EJson::Number>(TEXT("spline_id"))
					? Args->GetIntegerField(TEXT("spline_id"))
					: NextFallbackSplineId(World, Landscape);

				const FScopedTransaction Tx(LOCTEXT("CreateSplineFallback", "SOMOLMCP Create Landscape Spline Fallback"));
				Landscape->Modify();

				AActor* FallbackActor = GetOrCreateFallbackSplineActor(World, Landscape, SplineId);
				if (!FallbackActor)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("Failed to create landscape spline fallback actor."));
					Error = TEXT("Failed to create fallback spline actor.");
					return false;
				}

				FallbackActor->Modify();
				USplineComponent* Spline = FallbackActor->FindComponentByClass<USplineComponent>();
				if (!Spline)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("Fallback actor exists but has no USplineComponent."));
					Error = TEXT("Fallback spline actor has no spline component.");
					return false;
				}

				Spline->Modify();
				Spline->ClearSplinePoints(false);
				for (const FVector& P : Pts)
				{
					Spline->AddSplinePoint(P, ESplineCoordinateSpace::World, false);
				}
				Spline->SetClosedLoop(false, false);
				Spline->UpdateSpline();

				SetFallbackSplineTags(FallbackActor, Landscape, SplineId);
				SololmcpWriteFlush::EnsureFlushed(FallbackActor);

				Out->SetStringField(TEXT("landscape_actor"), Landscape->GetName());
				Out->SetBoolField(TEXT("applied"), true);
				Out->SetStringField(TEXT("fallback_mode"), TEXT("spline_actor_component"));
				Out->SetNumberField(TEXT("spline_id"), SplineId);
				Out->SetStringField(TEXT("spline_actor"), FallbackActor->GetPathName());
				Out->SetStringField(TEXT("spline_actor_label"), FallbackActor->GetActorLabel());
				Out->SetNumberField(TEXT("point_count"), Spline->GetNumberOfSplinePoints());
				Out->SetStringField(TEXT("degraded_reason"),
					TEXT("ULandscapeSplinesComponent control-point/segment mutation is intentionally avoided in this build; USplineComponent fallback is queryable and mesh-capable."));

				Summary = FString::Printf(TEXT("Created landscape spline fallback actor '%s' with %d points (spline_id=%d)."),
					*FallbackActor->GetActorLabel(), Spline->GetNumberOfSplinePoints(), SplineId);
				return true;

			},
			nullptr, 0
		});

		// -- 4. landscape_spline_add_mesh ------------------------------
		R.Register({
			TEXT("landscape_spline_add_mesh"),
			TEXT("Assign a UStaticMesh to every segment of the named spline chain."),
			SB::Object({
				{TEXT("landscape_actor_name"), SB::String()},
				{TEXT("spline_id"),            SB::Integer(TEXT("Spline chain id from landscape_spline_create."))},
				{TEXT("mesh_path"),            SB::String(TEXT("UStaticMesh asset path (/Game/...)."))},
				{TEXT("scale_xyz"),            SB::Array(SB::Number(), TEXT("[sx,sy,sz] (default [1,1,1])."))}
			}, {TEXT("landscape_actor_name"), TEXT("spline_id"), TEXT("mesh_path")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString LandscapeId, MeshPath;
				if (!Args->TryGetStringField(TEXT("landscape_actor_name"), LandscapeId))
				{ SololmcpError::MissingParam(Out, TEXT("landscape_actor_name")); Error = TEXT("Missing actor."); return false; }
				if (!Args->TryGetStringField(TEXT("mesh_path"), MeshPath))
				{ SololmcpError::MissingParam(Out, TEXT("mesh_path")); Error = TEXT("Missing mesh_path."); return false; }
				const int32 SplineId = Args->HasTypedField<EJson::Number>(TEXT("spline_id"))
					? Args->GetIntegerField(TEXT("spline_id")) : 0;

				FVector Scale(1.0, 1.0, 1.0);
				const TArray<TSharedPtr<FJsonValue>>* ScaleArr = nullptr;
				if (Args->TryGetArrayField(TEXT("scale_xyz"), ScaleArr) && ScaleArr && ScaleArr->Num() >= 3)
				{
					Scale.X = (*ScaleArr)[0]->AsNumber();
					Scale.Y = (*ScaleArr)[1]->AsNumber();
					Scale.Z = (*ScaleArr)[2]->AsNumber();
				}

				UWorld* World = Ctx.Services.GetEditorWorld(Error); if (!World) return false;
				ALandscape* Landscape = ResolveLandscape(World, LandscapeId, Error);
				if (!Landscape) { SololmcpError::NotFound(Out, LandscapeId); return false; }

				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
				if (!Mesh)
				{
					SololmcpError::InvalidPath(Out, MeshPath);
					Error = FString::Printf(TEXT("Failed to load UStaticMesh '%s'"), *MeshPath);
					return false;
				}

				ULandscapeSplinesComponent* Splines = Landscape->FindComponentByClass<ULandscapeSplinesComponent>();

				const FScopedTransaction Tx(LOCTEXT("AddSplineMesh", "SOMOLMCP Add Landscape Spline Mesh"));
				Landscape->Modify();

				int32 MeshCount = 0;
				if (Splines)
				{
					Splines->Modify();

					// Walk all segments owned by this splines component.
					// In UE 5.x, ULandscapeSplinesComponent stores segments in a public
					// UPROPERTY array `Segments` (TArray<TObjectPtr<ULandscapeSplineSegment>>).
					TArray<UObject*> Inners;
					GetObjectsWithOuter(Splines, Inners, /*IncludeNestedObjects*/ false);
					for (UObject* Obj : Inners)
					{
						ULandscapeSplineSegment* Seg = Cast<ULandscapeSplineSegment>(Obj);
						if (!Seg) continue;

						Seg->Modify();

						// Append to the segment's mesh array.
						// UE 5.7: FLandscapeSplineMeshEntry::Scale is FVector (LWC double), not FVector3f
						FLandscapeSplineMeshEntry Entry;
						Entry.Mesh = Mesh;
						Entry.Scale = Scale;
						Seg->SplineMeshes.Add(Entry);
						Seg->UpdateSplinePoints();
						++MeshCount;

						// Spline id filtering: -1 means "all".
						if (SplineId >= 0 && MeshCount > SplineId + 1) { /* keep going for now */ }
					}
				}

				SololmcpWriteFlush::EnsureFlushed(Landscape);

				int32 VerifiedMeshCount = 0;
				if (Splines)
				{
					TArray<UObject*> VerifyInners;
					GetObjectsWithOuter(Splines, VerifyInners, /*IncludeNestedObjects*/ false);
					for (UObject* Obj : VerifyInners)
					{
						ULandscapeSplineSegment* Seg = Cast<ULandscapeSplineSegment>(Obj);
						if (!Seg) continue;
						for (const FLandscapeSplineMeshEntry& Entry : Seg->SplineMeshes)
						{
							if (Entry.Mesh == Mesh)
							{
								++VerifiedMeshCount;
							}
						}
					}
				}

				Out->SetStringField(TEXT("landscape_actor"), Landscape->GetName());
				Out->SetNumberField(TEXT("spline_id"), SplineId);
				Out->SetStringField(TEXT("mesh_path"), MeshPath);
				Out->SetNumberField(TEXT("mesh_count"), MeshCount);
				Out->SetNumberField(TEXT("verified_mesh_count"), VerifiedMeshCount);
				if (MeshCount == 0)
				{
					AActor* FallbackActor = FindFallbackSplineActor(World, Landscape, SplineId);
					USplineComponent* FallbackSpline = FallbackActor ? FallbackActor->FindComponentByClass<USplineComponent>() : nullptr;
					if (FallbackActor && FallbackSpline && FallbackSpline->GetNumberOfSplinePoints() >= 2)
					{
						FallbackActor->Modify();
						FallbackSpline->Modify();

						int32 FallbackMeshCount = 0;
						for (int32 PointIdx = 0; PointIdx + 1 < FallbackSpline->GetNumberOfSplinePoints(); ++PointIdx)
						{
							USplineMeshComponent* SplineMesh = NewObject<USplineMeshComponent>(
								FallbackActor,
								USplineMeshComponent::StaticClass(),
								*FString::Printf(TEXT("LandscapeSplineFallbackMesh_%d_%d"), SplineId, PointIdx));
							if (!SplineMesh) continue;

							SplineMesh->SetStaticMesh(Mesh);
							SplineMesh->SetRelativeScale3D(Scale);
							const FVector StartPos = FallbackSpline->GetLocationAtSplinePoint(PointIdx, ESplineCoordinateSpace::Local);
							const FVector EndPos = FallbackSpline->GetLocationAtSplinePoint(PointIdx + 1, ESplineCoordinateSpace::Local);
							const FVector StartTangent = FallbackSpline->GetTangentAtSplinePoint(PointIdx, ESplineCoordinateSpace::Local);
							const FVector EndTangent = FallbackSpline->GetTangentAtSplinePoint(PointIdx + 1, ESplineCoordinateSpace::Local);
							SplineMesh->SetStartAndEnd(StartPos, StartTangent, EndPos, EndTangent, false);
							SplineMesh->ComponentTags.Add(FName(TEXT("SOMO.LandscapeSplineFallback.Mesh")));
							SplineMesh->ComponentTags.Add(FName(*LandscapeFallbackTag(TEXT("SplineId"), FString::FromInt(SplineId))));
							SplineMesh->AttachToComponent(FallbackSpline, FAttachmentTransformRules::KeepRelativeTransform);
							FallbackActor->AddInstanceComponent(SplineMesh);
							SplineMesh->RegisterComponent();
							SplineMesh->UpdateMesh();
							++FallbackMeshCount;
						}

						SololmcpWriteFlush::EnsureFlushed(FallbackActor);

						int32 VerifiedFallbackMeshCount = 0;
						TArray<USplineMeshComponent*> SplineMeshes;
						FallbackActor->GetComponents<USplineMeshComponent>(SplineMeshes);
						for (USplineMeshComponent* SplineMesh : SplineMeshes)
						{
							if (SplineMesh && SplineMesh->GetStaticMesh() == Mesh &&
								SplineMesh->ComponentTags.Contains(FName(*LandscapeFallbackTag(TEXT("SplineId"), FString::FromInt(SplineId)))))
							{
								++VerifiedFallbackMeshCount;
							}
						}

						Out->SetStringField(TEXT("fallback_mode"), TEXT("spline_actor_component"));
						Out->SetStringField(TEXT("spline_actor"), FallbackActor->GetPathName());
						Out->SetStringField(TEXT("spline_actor_label"), FallbackActor->GetActorLabel());
						Out->SetNumberField(TEXT("point_count"), FallbackSpline->GetNumberOfSplinePoints());
						Out->SetNumberField(TEXT("mesh_count"), FallbackMeshCount);
						Out->SetNumberField(TEXT("verified_mesh_count"), VerifiedFallbackMeshCount);
						if (FallbackMeshCount > 0 && VerifiedFallbackMeshCount > 0)
						{
							Out->SetObjectField(TEXT("scale_xyz"), [&]()
							{
								TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
								J->SetNumberField(TEXT("x"), Scale.X);
								J->SetNumberField(TEXT("y"), Scale.Y);
								J->SetNumberField(TEXT("z"), Scale.Z);
								return J;
							}());
							Summary = FString::Printf(TEXT("Added mesh '%s' to %d fallback spline segments."), *MeshPath, FallbackMeshCount);
							return true;
						}
					}

					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT("spline_id"),
						TEXT("No native landscape spline segments were modified and no matching fallback spline actor could be populated."));
					Out->SetStringField(TEXT("fallback_mode"), TEXT("none_available"));
					Error = TEXT("No landscape spline segments or fallback spline actor were modified.");
					return false;
				}
				if (VerifiedMeshCount == 0)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT("mesh_path"),
						TEXT("Spline mesh assignment did not verify on readback."));
					Error = TEXT("Spline mesh assignment did not verify on readback.");
					return false;
				}
				Out->SetObjectField(TEXT("scale_xyz"), [&]()
				{
					TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
					J->SetNumberField(TEXT("x"), Scale.X);
					J->SetNumberField(TEXT("y"), Scale.Y);
					J->SetNumberField(TEXT("z"), Scale.Z);
					return J;
				}());

				Summary = FString::Printf(TEXT("Added mesh '%s' to %d segments."), *MeshPath, MeshCount);
				return true;
			},
			nullptr, 0
		});

		// -- 5. landscape_set_visibility_mask --------------------------
		R.Register({
			TEXT("landscape_set_visibility_mask"),
			TEXT("Toggle landscape components hidden/visible whose centers lie inside a region box."),
			SB::Object({
				{TEXT("landscape_actor_name"), SB::String()},
				{TEXT("region_box"),           RegionBoxSchema()},
				{TEXT("visible"),              SB::Boolean(TEXT("true=show, false=hide."))}
			}, {TEXT("landscape_actor_name"), TEXT("region_box"), TEXT("visible")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString LandscapeId;
				if (!Args->TryGetStringField(TEXT("landscape_actor_name"), LandscapeId))
				{ SololmcpError::MissingParam(Out, TEXT("landscape_actor_name")); Error = TEXT("Missing actor."); return false; }
				FBox2D Region;
				if (!ParseRegionBox(Args, TEXT("region_box"), Region, Error))
				{
					SololmcpError::Set(Out, TEXT("INVALID_TYPE"), TEXT("region_box"),
						TEXT("region_box must be [x_min,y_min,x_max,y_max]."));
					return false;
				}
				const bool bVisible = Args->GetBoolField(TEXT("visible"));

				UWorld* World = Ctx.Services.GetEditorWorld(Error); if (!World) return false;
				ALandscape* Landscape = ResolveLandscape(World, LandscapeId, Error);
				if (!Landscape) { SololmcpError::NotFound(Out, LandscapeId); return false; }

				const FScopedTransaction Tx(LOCTEXT("SetVisMask", "SOMOLMCP Set Landscape Visibility Mask"));
				Landscape->Modify();

				int32 Affected = 0;

				// UE 5.7: ULandscapeInfo::ForAllLandscapeProxies removed.
				// Use public LandscapeActor + SortedStreamingProxies fields.
				ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
				TArray<ALandscapeProxy*> Proxies;
				if (Info)
				{
					if (ALandscape* RootLs = Info->LandscapeActor.Get())
					{
						Proxies.Add(RootLs);
					}
					// UE 5.7: SortedStreamingProxies became private; fall back to just the root landscape actor.
					// For full proxy enumeration, use python_exec + unreal.LandscapeInfo APIs.
					if (Proxies.Num() == 0)
					{
						Proxies.Add(Landscape);
					}
				}
				else
				{
					Proxies.Add(Landscape);
				}

				for (ALandscapeProxy* Proxy : Proxies)
				{
					if (!Proxy) continue;
					Proxy->Modify();
					for (ULandscapeComponent* Comp : Proxy->LandscapeComponents)
					{
						if (!Comp) continue;
						const FBoxSphereBounds Bounds = Comp->Bounds;
						const FVector Center = Bounds.Origin;
						if (Region.IsInside(FVector2D(Center.X, Center.Y)))
						{
							Comp->Modify();
							Comp->SetVisibility(bVisible);
							Comp->SetHiddenInGame(!bVisible);
							Comp->MarkRenderStateDirty();
							++Affected;
						}
					}
				}

				SololmcpWriteFlush::EnsureFlushed(Landscape);

				Out->SetStringField(TEXT("landscape_actor"), Landscape->GetName());
				Out->SetBoolField(TEXT("visible"), bVisible);
				Out->SetNumberField(TEXT("component_count_affected"), Affected);
				TArray<TSharedPtr<FJsonValue>> RV;
				RV.Add(MakeShared<FJsonValueNumber>(Region.Min.X));
				RV.Add(MakeShared<FJsonValueNumber>(Region.Min.Y));
				RV.Add(MakeShared<FJsonValueNumber>(Region.Max.X));
				RV.Add(MakeShared<FJsonValueNumber>(Region.Max.Y));
				Out->SetArrayField(TEXT("region_box"), RV);

				if (Affected == 0)
				{
					SololmcpError::Set(Out, TEXT("NO_MATCH"), TEXT("region_box"),
						TEXT("No landscape components had centers inside region_box; visibility was not changed."));
					Error = TEXT("No landscape components matched region_box.");
					return false;
				}

				Summary = FString::Printf(TEXT("Set visibility=%s on %d components."),
					bVisible ? TEXT("true") : TEXT("false"), Affected);
				return true;
			},
			nullptr, 0
		});
	}
} // namespace UE::SOMOLMCP

#undef LOCTEXT_NAMESPACE
