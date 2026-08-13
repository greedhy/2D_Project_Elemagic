// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Tools/SololmcpToolRegistry.h"

#include "SololmcpErrorHelpers.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpWriteFlush.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Navigation/NavLinkProxy.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/HLOD/HLODLayer.h"

namespace UE::SOMOLMCP
{
namespace ArchitectureTools
{
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

	static TSharedRef<FJsonObject> VectorJson(const FVector& Value)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static int32 CountSimpleGeoms(const UBodySetup* BodySetup)
	{
		if (!BodySetup)
		{
			return 0;
		}
		return BodySetup->AggGeom.BoxElems.Num()
			+ BodySetup->AggGeom.SphereElems.Num()
			+ BodySetup->AggGeom.SphylElems.Num()
			+ BodySetup->AggGeom.ConvexElems.Num();
	}

	static UStaticMesh* LoadStaticMesh(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		FString AssetPath;
		if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
		{
			OutStructured->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
			OutError = TEXT("Missing asset_path.");
			return nullptr;
		}
		if (!FPackageName::IsValidObjectPath(AssetPath) && !FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutStructured->SetBoolField(TEXT("ok"), false);
			SololmcpError::InvalidPath(OutStructured, AssetPath);
			OutError = FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath);
			return nullptr;
		}

		UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
		if (!Asset)
		{
			OutStructured->SetBoolField(TEXT("ok"), false);
			SololmcpError::InvalidPath(OutStructured, AssetPath);
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Failed to load asset '%s'."), *AssetPath);
			}
			return nullptr;
		}

		UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
		if (!StaticMesh)
		{
			OutStructured->SetBoolField(TEXT("ok"), false);
			SololmcpError::InvalidType(OutStructured, TEXT("asset_path"), TEXT("UStaticMesh"));
			OutError = TEXT("Asset is not a UStaticMesh.");
			return nullptr;
		}
		return StaticMesh;
	}

	static UWorld* GetEditorWorld(
		const FSololmcpToolExecutionContext& Context,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		UWorld* World = Context.Services.GetEditorWorld(OutError);
		if (!World)
		{
			OutStructured->SetBoolField(TEXT("ok"), false);
			SololmcpError::Set(OutStructured, TEXT("NO_EDITOR_WORLD"), TEXT(""), OutError.IsEmpty() ? TEXT("Editor world is unavailable.") : OutError);
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Editor world is unavailable.");
			}
		}
		return World;
	}

	static UStaticMesh* LoadStaticMeshByPath(
		const FSololmcpToolExecutionContext& Context,
		const FString& AssetPath,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		if (AssetPath.TrimStartAndEnd().IsEmpty())
		{
			OutStructured->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
			OutError = TEXT("Missing asset_path.");
			return nullptr;
		}
		if (!FPackageName::IsValidObjectPath(AssetPath) && !FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutStructured->SetBoolField(TEXT("ok"), false);
			SololmcpError::InvalidPath(OutStructured, AssetPath);
			OutError = FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath);
			return nullptr;
		}

		UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
		if (!StaticMesh)
		{
			OutStructured->SetBoolField(TEXT("ok"), false);
			SololmcpError::InvalidType(OutStructured, TEXT("asset_path"), TEXT("UStaticMesh"));
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Asset '%s' is not a UStaticMesh or failed to load."), *AssetPath);
			}
			return nullptr;
		}
		return StaticMesh;
	}

	static bool TryReadVectorArray(const TSharedPtr<FJsonValue>& Value, FVector& Out)
	{
		if (!Value.IsValid() || Value->Type != EJson::Array)
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
		if (Array.Num() < 3 || !Array[0].IsValid() || !Array[1].IsValid() || !Array[2].IsValid())
		{
			return false;
		}
		Out = FVector(Array[0]->AsNumber(), Array[1]->AsNumber(), Array[2]->AsNumber());
		return true;
	}

	static bool TryReadVectorObject(const TSharedPtr<FJsonObject>& Object, FVector& Out)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		double X = Out.X;
		double Y = Out.Y;
		double Z = Out.Z;
		Object->TryGetNumberField(TEXT("x"), X);
		Object->TryGetNumberField(TEXT("y"), Y);
		Object->TryGetNumberField(TEXT("z"), Z);
		Out = FVector(X, Y, Z);
		return true;
	}

	static bool TryReadVectorField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& Out)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		if (const TSharedPtr<FJsonObject>* Child = nullptr; Object->TryGetObjectField(FieldName, Child) && Child && Child->IsValid())
		{
			return TryReadVectorObject(*Child, Out);
		}
		if (const TSharedPtr<FJsonValue> Value = Object->TryGetField(FieldName))
		{
			return TryReadVectorArray(Value, Out);
		}
		return false;
	}

	static bool TryReadRotatorObject(const TSharedPtr<FJsonObject>& Object, FRotator& Out)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		double Pitch = Out.Pitch;
		double Yaw = Out.Yaw;
		double Roll = Out.Roll;
		Object->TryGetNumberField(TEXT("pitch"), Pitch);
		Object->TryGetNumberField(TEXT("yaw"), Yaw);
		Object->TryGetNumberField(TEXT("roll"), Roll);
		Out = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	static bool TryReadRotatorField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FRotator& Out)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		if (const TSharedPtr<FJsonObject>* Child = nullptr; Object->TryGetObjectField(FieldName, Child) && Child && Child->IsValid())
		{
			return TryReadRotatorObject(*Child, Out);
		}
		if (const TSharedPtr<FJsonValue> Value = Object->TryGetField(FieldName))
		{
			FVector AsVector(Out.Pitch, Out.Yaw, Out.Roll);
			if (TryReadVectorArray(Value, AsVector))
			{
				Out = FRotator(AsVector.X, AsVector.Y, AsVector.Z);
				return true;
			}
		}
		return false;
	}

	static bool TryReadScaleField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& Out)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		double Scalar = 0.0;
		if (Object->TryGetNumberField(FieldName, Scalar))
		{
			Out = FVector(Scalar);
			return true;
		}
		return TryReadVectorField(Object, FieldName, Out);
	}

	static FString MakeReceiptHash(const FString& Payload)
	{
		return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*Payload));
	}

	static bool JsonObjectContainsAnyToken(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Tokens);
	static bool ReadStringFieldAny(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, FString& Out);
	static bool ResolveModuleDescriptors(const TSharedRef<FJsonObject>& Arguments, const TArray<TSharedPtr<FJsonValue>>*& OutModules);
	static FString ReadModuleAssetPath(const TSharedPtr<FJsonObject>& Module);

	static FString MakeUniqueActorLabel(UWorld* World, const FString& DesiredLabel)
	{
		FString Base = DesiredLabel.TrimStartAndEnd();
		if (Base.IsEmpty())
		{
			Base = TEXT("SOMOL_Architecture_Module");
		}

		auto LabelExists = [World](const FString& Candidate)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetActorLabel().Equals(Candidate, ESearchCase::IgnoreCase) || It->GetName().Equals(Candidate, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		};

		if (!LabelExists(Base))
		{
			return Base;
		}
		for (int32 Index = 2; Index < 100000; ++Index)
		{
			const FString Candidate = FString::Printf(TEXT("%s_%03d"), *Base, Index);
			if (!LabelExists(Candidate))
			{
				return Candidate;
			}
		}
		return FString::Printf(TEXT("%s_%08x"), *Base, FCrc::StrCrc32(*Base));
	}

	static TArray<FString> SocketNames(const UStaticMesh* StaticMesh)
	{
		TArray<FString> Names;
		if (!StaticMesh)
		{
			return Names;
		}
		for (const UStaticMeshSocket* Socket : StaticMesh->Sockets)
		{
			if (Socket)
			{
				Names.Add(Socket->SocketName.ToString());
			}
		}
		return Names;
	}

	static bool HasSocketNamed(const UStaticMesh* StaticMesh, const FString& SocketName)
	{
		if (!StaticMesh || SocketName.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}
		for (const UStaticMeshSocket* Socket : StaticMesh->Sockets)
		{
			if (Socket && Socket->SocketName.ToString().Equals(SocketName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static bool TryGetStringArrayField(const TSharedRef<FJsonObject>& Object, const TCHAR* FieldName, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}
		Out.Reset();
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (Value.IsValid())
			{
				const FString Text = Value->AsString().TrimStartAndEnd();
				if (!Text.IsEmpty())
				{
					Out.Add(Text);
				}
			}
		}
		return !Out.IsEmpty();
	}

	static TSharedRef<FJsonObject> StaticMeshSummary(UStaticMesh* StaticMesh)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		const FBoxSphereBounds Bounds = StaticMesh->GetBounds();
		const UBodySetup* BodySetup = StaticMesh->GetBodySetup();
		const int32 SimpleGeomCount = CountSimpleGeoms(BodySetup);
		Summary->SetStringField(TEXT("asset_name"), StaticMesh->GetName());
		Summary->SetNumberField(TEXT("lod_count"), StaticMesh->GetNumLODs());
		Summary->SetNumberField(TEXT("material_slot_count"), StaticMesh->GetStaticMaterials().Num());
		Summary->SetObjectField(TEXT("bounds_origin_cm"), VectorJson(Bounds.Origin));
		Summary->SetObjectField(TEXT("bounds_extent_cm"), VectorJson(Bounds.BoxExtent));
		Summary->SetBoolField(TEXT("has_body_setup"), BodySetup != nullptr);
		Summary->SetNumberField(TEXT("simple_collision_geom_count"), SimpleGeomCount);
		Summary->SetBoolField(TEXT("has_simple_collision"), SimpleGeomCount > 0);
		Summary->SetArrayField(TEXT("sockets"), StringArrayJson(SocketNames(StaticMesh)));
#if WITH_EDITORONLY_DATA
		Summary->SetBoolField(TEXT("nanite_enabled"), SOMOLMCP_NANITE_SETTINGS(StaticMesh).bEnabled);
#else
		Summary->SetBoolField(TEXT("nanite_enabled"), false);
#endif
		return Summary;
	}

	static TSharedRef<FJsonObject> ActorReadbackJson(AActor* Actor)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), IsValid(Actor));
		if (!IsValid(Actor))
		{
			return Obj;
		}

		Obj->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		Obj->SetStringField(TEXT("actor_name"), Actor->GetName());
		Obj->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		TArray<FString> ActorTags;
		for (const FName& Tag : Actor->Tags)
		{
			ActorTags.Add(Tag.ToString());
		}
		Obj->SetArrayField(TEXT("tags"), StringArrayJson(ActorTags));
		Obj->SetObjectField(TEXT("location_cm"), VectorJson(Actor->GetActorLocation()));
		const FRotator Rotation = Actor->GetActorRotation();
		TSharedRef<FJsonObject> Rot = MakeShared<FJsonObject>();
		Rot->SetNumberField(TEXT("pitch"), Rotation.Pitch);
		Rot->SetNumberField(TEXT("yaw"), Rotation.Yaw);
		Rot->SetNumberField(TEXT("roll"), Rotation.Roll);
		Obj->SetObjectField(TEXT("rotation_deg"), Rot);
		Obj->SetObjectField(TEXT("scale"), VectorJson(Actor->GetActorScale3D()));

		TArray<UStaticMeshComponent*> Components;
		Actor->GetComponents<UStaticMeshComponent>(Components);
		TArray<TSharedPtr<FJsonValue>> ComponentJson;
		int32 CollisionEnabledCount = 0;
		int32 NavRelevantCount = 0;
		int32 SimpleGeomCount = 0;
		for (UStaticMeshComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("component_name"), Component->GetName());
			C->SetStringField(TEXT("collision_enabled"), StaticEnum<ECollisionEnabled::Type>()->GetNameStringByValue(static_cast<int64>(Component->GetCollisionEnabled())));
			C->SetStringField(TEXT("collision_profile"), Component->GetCollisionProfileName().ToString());
			C->SetBoolField(TEXT("can_ever_affect_navigation"), Component->CanEverAffectNavigation());
			if (Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
			{
				++CollisionEnabledCount;
			}
			if (Component->CanEverAffectNavigation())
			{
				++NavRelevantCount;
			}
			if (const UStaticMesh* Mesh = Component->GetStaticMesh())
			{
				C->SetStringField(TEXT("static_mesh"), Mesh->GetPathName());
				const int32 MeshSimpleGeomCount = CountSimpleGeoms(Mesh->GetBodySetup());
				C->SetNumberField(TEXT("simple_collision_geom_count"), MeshSimpleGeomCount);
				SimpleGeomCount += MeshSimpleGeomCount;
			}
			ComponentJson.Add(MakeShared<FJsonValueObject>(C));
		}
		Obj->SetArrayField(TEXT("static_mesh_components"), ComponentJson);
		Obj->SetNumberField(TEXT("static_mesh_component_count"), Components.Num());
		Obj->SetNumberField(TEXT("collision_enabled_component_count"), CollisionEnabledCount);
		Obj->SetNumberField(TEXT("nav_relevant_component_count"), NavRelevantCount);
		Obj->SetNumberField(TEXT("simple_collision_geom_count"), SimpleGeomCount);
		return Obj;
	}

	static void AppendActorLabelsFromArray(const TArray<TSharedPtr<FJsonValue>>* Values, TArray<FString>& OutLabels)
	{
		if (!Values)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			const FString Label = Value->AsString().TrimStartAndEnd();
			if (!Label.IsEmpty())
			{
				OutLabels.AddUnique(Label);
			}
		}
	}

	static void CollectActorLabelsFromReceipt(const TSharedPtr<FJsonObject>& Receipt, TArray<FString>& OutLabels)
	{
		if (!Receipt.IsValid())
		{
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* SpawnedActors = nullptr;
		if (!Receipt->TryGetArrayField(TEXT("spawned_actors"), SpawnedActors) || !SpawnedActors)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& ActorValue : *SpawnedActors)
		{
			const TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
			if (!ActorObj.IsValid())
			{
				continue;
			}
			FString Label;
			if ((ActorObj->TryGetStringField(TEXT("actor_label"), Label) || ActorObj->TryGetStringField(TEXT("name"), Label)) && !Label.TrimStartAndEnd().IsEmpty())
			{
				OutLabels.AddUnique(Label.TrimStartAndEnd());
			}
		}
	}

	static void ResolveArchitectureActors(
		UWorld* World,
		const TSharedRef<FJsonObject>& Arguments,
		TArray<AActor*>& OutActors,
		FString& OutAssemblyId)
	{
		OutActors.Reset();
		TArray<FString> Labels;
		const TArray<TSharedPtr<FJsonValue>>* LabelArray = nullptr;
		if (Arguments->TryGetArrayField(TEXT("actor_labels"), LabelArray))
		{
			AppendActorLabelsFromArray(LabelArray, Labels);
		}

		if (const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr; Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) && ReceiptPtr && ReceiptPtr->IsValid())
		{
			CollectActorLabelsFromReceipt(*ReceiptPtr, Labels);
			if (OutAssemblyId.IsEmpty())
			{
				(*ReceiptPtr)->TryGetStringField(TEXT("assembly_id"), OutAssemblyId);
			}
		}

		Arguments->TryGetStringField(TEXT("assembly_id"), OutAssemblyId);
		const FName AssemblyTag = OutAssemblyId.IsEmpty()
			? NAME_None
			: FName(*FString::Printf(TEXT("SOMOLArchitectureAssembly:%s"), *OutAssemblyId));

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			const bool bLabelMatch = Labels.ContainsByPredicate([Actor](const FString& Label)
			{
				return Actor->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)
					|| Actor->GetName().Equals(Label, ESearchCase::IgnoreCase)
					|| Actor->GetPathName().Equals(Label, ESearchCase::IgnoreCase);
			});
			const bool bAssemblyMatch = AssemblyTag != NAME_None && Actor->Tags.Contains(AssemblyTag);
			if (bLabelMatch || bAssemblyMatch)
			{
				OutActors.AddUnique(Actor);
			}
		}
	}

	static void SetArchitectureReceiptBase(const TSharedRef<FJsonObject>& Out, const FString& ToolName)
	{
		Out->SetStringField(TEXT("schema"), FString::Printf(TEXT("somol.%s.receipt.v1"), *ToolName));
		Out->SetStringField(TEXT("tool"), ToolName);
		Out->SetStringField(TEXT("domain"), TEXT("architecture"));
		Out->SetBoolField(TEXT("receipt_envelope_required"), true);
		Out->SetArrayField(TEXT("hard_gates"), StringArrayJson({
			TEXT("target_guard"),
			TEXT("readback"),
			TEXT("collision_audit"),
			TEXT("interior_nav_or_explicit_non_enterable_flag"),
			TEXT("reachability_audit"),
			TEXT("screenshot_or_viewport_receipt")
		}));
	}

	static TSharedRef<FJsonObject> MakeArchitectureFollowupStep(
		int32 Order,
		const FString& Tool,
		const FString& Lane,
		const FString& Purpose)
	{
		TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetNumberField(TEXT("order"), Order);
		Step->SetStringField(TEXT("tool"), Tool);
		Step->SetStringField(TEXT("lane"), Lane);
		Step->SetStringField(TEXT("purpose"), Purpose);
		Step->SetBoolField(TEXT("mcp_required"), true);
		Step->SetObjectField(TEXT("args_template"), MakeShared<FJsonObject>());
		return Step;
	}

	static TSharedPtr<FJsonObject> FollowupArgs(const TSharedRef<FJsonObject>& Step)
	{
		return Step->GetObjectField(TEXT("args_template"));
	}

	static TArray<TSharedPtr<FJsonValue>> BuildArchitectureWorldForgeFollowupPlan()
	{
		TArray<TSharedPtr<FJsonValue>> Steps;

		TSharedRef<FJsonObject> DataLayer = MakeArchitectureFollowupStep(
			1,
			TEXT("world_create_data_layer_actor_membership_apply"),
			TEXT("level_write"),
			TEXT("Apply native DataLayer membership for architecture actors after deployment tag readback."));
		FollowupArgs(DataLayer)->SetStringField(TEXT("execute"), TEXT("$input.execute"));
		FollowupArgs(DataLayer)->SetStringField(TEXT("data_layer_name"), TEXT("$input.data_layer_name"));
		FollowupArgs(DataLayer)->SetStringField(TEXT("actor_labels"), TEXT("$architecture_deployment_metadata_apply.actors[].actor_label"));
		FollowupArgs(DataLayer)->SetStringField(TEXT("receipt"), TEXT("$architecture_deployment_metadata_apply"));
		Steps.Add(MakeShared<FJsonValueObject>(DataLayer));

		TSharedRef<FJsonObject> HlodDispatch = MakeArchitectureFollowupStep(
			2,
			TEXT("architecture_hlod_build_dispatch"),
			TEXT("editor_build"),
			TEXT("Dispatch HLOD build after deployment metadata and DataLayer membership are available."));
		FollowupArgs(HlodDispatch)->SetStringField(TEXT("execute"), TEXT("$input.execute"));
		FollowupArgs(HlodDispatch)->SetStringField(TEXT("mode"), TEXT("changed"));
		Steps.Add(MakeShared<FJsonValueObject>(HlodDispatch));

		TSharedRef<FJsonObject> HlodPoll = MakeArchitectureFollowupStep(
			3,
			TEXT("world_create_hlod_job_poll"),
			TEXT("poll"),
			TEXT("Poll or read back HLOD completion evidence when a WorldCreate HLOD job receipt exists."));
		FollowupArgs(HlodPoll)->SetStringField(TEXT("job_id"), TEXT("$architecture_hlod_build_dispatch.job_id"));
		FollowupArgs(HlodPoll)->SetStringField(TEXT("receipt"), TEXT("$architecture_hlod_build_dispatch"));
		Steps.Add(MakeShared<FJsonValueObject>(HlodPoll));

		TSharedRef<FJsonObject> Save = MakeArchitectureFollowupStep(
			4,
			TEXT("world_create_save_validate_fast"),
			TEXT("save"),
			TEXT("Save and validate dirty packages touched by the architecture deployment."));
		FollowupArgs(Save)->SetStringField(TEXT("execute"), TEXT("$input.execute"));
		FollowupArgs(Save)->SetStringField(TEXT("target_receipt"), TEXT("$architecture_deployment_metadata_apply"));
		Steps.Add(MakeShared<FJsonValueObject>(Save));

		TSharedRef<FJsonObject> Reload = MakeArchitectureFollowupStep(
			5,
			TEXT("world_create_reload_verify"),
			TEXT("reload"),
			TEXT("Verify saved architecture world state can reload and still exposes deployment actors."));
		FollowupArgs(Reload)->SetStringField(TEXT("target_receipt"), TEXT("$world_create_save_validate_fast"));
		FollowupArgs(Reload)->SetStringField(TEXT("assembly_id"), TEXT("$architecture_deployment_metadata_apply.assembly_id"));
		Steps.Add(MakeShared<FJsonValueObject>(Reload));

		TSharedRef<FJsonObject> Screenshot = MakeArchitectureFollowupStep(
			6,
			TEXT("editor_screenshot_viewport"),
			TEXT("read"),
			TEXT("Capture viewport evidence for QA and production gate validation."));
		FollowupArgs(Screenshot)->SetStringField(TEXT("focus_actor_labels"), TEXT("$architecture_deployment_metadata_apply.actors[].actor_label"));
		Steps.Add(MakeShared<FJsonValueObject>(Screenshot));

		TSharedRef<FJsonObject> Gate = MakeArchitectureFollowupStep(
			7,
			TEXT("architecture_production_gate_validate"),
			TEXT("receipt_validate"),
			TEXT("Fail closed unless assembly, collision, reachability, nav, DataLayer, HLOD, save, reload, and screenshot receipts are present."));
		FollowupArgs(Gate)->SetStringField(TEXT("require_worldforge_chain"), TEXT("true"));
		FollowupArgs(Gate)->SetStringField(TEXT("prior_receipts"), TEXT("$all_prior_receipts"));
		Steps.Add(MakeShared<FJsonValueObject>(Gate));

		return Steps;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildArchitectureMissingGateFollowupPlan(const TArray<FString>& MissingGates)
	{
		TArray<TSharedPtr<FJsonValue>> Steps;
		int32 Order = 1;
		for (const FString& Gate : MissingGates)
		{
			const FString Normalized = Gate.ToLower();
			FString Tool = Gate;
			FString Lane = TEXT("receipt_validate");
			FString Purpose = FString::Printf(TEXT("Produce missing architecture production gate '%s' through MCP."), *Gate);
			if (Normalized == TEXT("assembly"))
			{
				Tool = TEXT("architecture_assembly_readback");
				Lane = TEXT("read");
			}
			else if (Normalized == TEXT("collision"))
			{
				Tool = TEXT("architecture_collision_audit");
				Lane = TEXT("read");
			}
			else if (Normalized == TEXT("reachability"))
			{
				Tool = TEXT("architecture_reachability_audit");
				Lane = TEXT("read");
			}
			else if (Normalized == TEXT("nav") || Normalized == TEXT("navmesh"))
			{
				Tool = TEXT("architecture_navmesh_path_sample");
				Lane = TEXT("read");
			}
			else if (Normalized == TEXT("data_layer"))
			{
				Tool = TEXT("world_create_data_layer_actor_membership_apply");
				Lane = TEXT("level_write");
			}
			else if (Normalized == TEXT("hlod"))
			{
				Tool = TEXT("architecture_hlod_build_dispatch");
				Lane = TEXT("editor_build");
			}
			else if (Normalized == TEXT("save"))
			{
				Tool = TEXT("world_create_save_validate_fast");
				Lane = TEXT("save");
			}
			else if (Normalized == TEXT("reload"))
			{
				Tool = TEXT("world_create_reload_verify");
				Lane = TEXT("reload");
			}
			else if (Normalized == TEXT("screenshot") || Normalized == TEXT("visual"))
			{
				Tool = TEXT("editor_screenshot_viewport");
				Lane = TEXT("read");
			}

			TSharedRef<FJsonObject> Step = MakeArchitectureFollowupStep(Order++, Tool, Lane, Purpose);
			FollowupArgs(Step)->SetStringField(TEXT("source_gate"), Gate);
			FollowupArgs(Step)->SetStringField(TEXT("prior_receipts"), TEXT("$all_prior_receipts"));
			Steps.Add(MakeShared<FJsonValueObject>(Step));
		}
		return Steps;
	}

	static bool RunArchitectureModuleValidate(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_module_validate"));
		UStaticMesh* StaticMesh = LoadStaticMesh(Context, Arguments, Out, Error);
		if (!StaticMesh)
		{
			return false;
		}

		const TSharedRef<FJsonObject> Mesh = StaticMeshSummary(StaticMesh);
		const bool bHasCollision = Mesh->GetBoolField(TEXT("has_simple_collision"));
		const int32 SocketCount = SocketNames(StaticMesh).Num();
		const bool bOk = bHasCollision && StaticMesh->GetNumLODs() > 0 && StaticMesh->GetStaticMaterials().Num() > 0;
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetObjectField(TEXT("module_profile"), Mesh);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("module_contract_valid") : TEXT("module_contract_incomplete"));
		Out->SetArrayField(TEXT("missing_capabilities"), StringArrayJson({
			bHasCollision ? TEXT("") : TEXT("simple_collision"),
			SocketCount > 0 ? TEXT("") : TEXT("sockets_optional_but_recommended")
		}));
		if (!bOk)
		{
			Error = TEXT("Architecture module is missing required collision, LOD, or material-slot evidence.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		Summary = FString::Printf(TEXT("Architecture module validated: %s."), *StaticMesh->GetName());
		return true;
	}

	static bool RunArchitectureSocketContractValidate(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_socket_contract_validate"));
		UStaticMesh* StaticMesh = LoadStaticMesh(Context, Arguments, Out, Error);
		if (!StaticMesh)
		{
			return false;
		}

		TArray<FString> RequiredSockets;
		TryGetStringArrayField(Arguments, TEXT("required_sockets"), RequiredSockets);
		if (RequiredSockets.IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error_code"), TEXT("MISSING_PARAM"));
			Out->SetStringField(TEXT("expected_param"), TEXT("required_sockets"));
			Error = TEXT("Missing required_sockets.");
			return false;
		}

		TArray<FString> Missing;
		for (const FString& RequiredSocket : RequiredSockets)
		{
			if (!HasSocketNamed(StaticMesh, RequiredSocket))
			{
				Missing.Add(RequiredSocket);
			}
		}

		const bool bOk = Missing.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("socket_contract_valid") : TEXT("socket_contract_failed"));
		Out->SetArrayField(TEXT("required_sockets"), StringArrayJson(RequiredSockets));
		Out->SetArrayField(TEXT("actual_sockets"), StringArrayJson(SocketNames(StaticMesh)));
		Out->SetArrayField(TEXT("missing_sockets"), StringArrayJson(Missing));
		if (!bOk)
		{
			Error = TEXT("Architecture socket contract failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		Summary = FString::Printf(TEXT("Architecture socket contract valid for %s."), *StaticMesh->GetName());
		return true;
	}

	static bool RunArchitectureKitCoverageAudit(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_kit_coverage_audit"));
		const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
		if (!Arguments->TryGetArrayField(TEXT("module_descriptors"), Modules) || !Modules)
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("module_descriptors"));
			Error = TEXT("Missing module_descriptors.");
			return false;
		}

		TArray<FString> RequiredKinds;
		if (!TryGetStringArrayField(Arguments, TEXT("required_kinds"), RequiredKinds))
		{
			RequiredKinds = { TEXT("wall"), TEXT("door"), TEXT("floor"), TEXT("roof"), TEXT("stair") };
		}

		TSet<FString> PresentKinds;
		for (const TSharedPtr<FJsonValue>& ModuleValue : *Modules)
		{
			const TSharedPtr<FJsonObject> Module = ModuleValue.IsValid() ? ModuleValue->AsObject() : nullptr;
			if (!Module.IsValid())
			{
				continue;
			}
			FString Kind;
			if (Module->TryGetStringField(TEXT("kind"), Kind) && !Kind.TrimStartAndEnd().IsEmpty())
			{
				PresentKinds.Add(Kind.ToLower());
			}
		}

		TArray<FString> MissingKinds;
		for (const FString& RequiredKind : RequiredKinds)
		{
			if (!PresentKinds.Contains(RequiredKind.ToLower()))
			{
				MissingKinds.Add(RequiredKind);
			}
		}

		const bool bOk = MissingKinds.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("kit_coverage_valid") : TEXT("kit_coverage_incomplete"));
		Out->SetNumberField(TEXT("module_count"), Modules->Num());
		Out->SetArrayField(TEXT("required_kinds"), StringArrayJson(RequiredKinds));
		Out->SetArrayField(TEXT("missing_kinds"), StringArrayJson(MissingKinds));
		if (!bOk)
		{
			Error = TEXT("Architecture kit coverage is incomplete.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		Summary = TEXT("Architecture kit coverage audit passed.");
		return true;
	}

	static bool RunArchitectureKitManifestValidate(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_kit_manifest_validate"));
		const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
		if (!ResolveModuleDescriptors(Arguments, Modules) || !Modules)
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("module_descriptors"));
			Error = TEXT("Missing module_descriptors, modular_kit_manifest.module_descriptors, or kit_modules.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<FString> Failures;
		TSet<FString> Kinds;
		int32 LoadedMeshes = 0;
		int32 CollisionReady = 0;
		int32 SocketReady = 0;
		for (int32 Index = 0; Index < Modules->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Module = (*Modules)[Index].IsValid() ? (*Modules)[Index]->AsObject() : nullptr;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), Index);
			FString Kind;
			ReadStringFieldAny(Module, { TEXT("kind"), TEXT("module_kind"), TEXT("role") }, Kind);
			Kind = Kind.ToLower();
			Row->SetStringField(TEXT("kind"), Kind);
			if (!Kind.IsEmpty())
			{
				Kinds.Add(Kind);
			}
			const FString AssetPath = ReadModuleAssetPath(Module);
			Row->SetStringField(TEXT("asset_path"), AssetPath);
			TArray<FString> RowFailures;
			if (Kind.IsEmpty())
			{
				RowFailures.Add(TEXT("missing_kind"));
			}
			if (AssetPath.IsEmpty())
			{
				RowFailures.Add(TEXT("missing_asset_path"));
			}
			else if (!FPackageName::IsValidObjectPath(AssetPath) && !FPackageName::IsValidLongPackageName(AssetPath))
			{
				RowFailures.Add(TEXT("invalid_asset_path"));
			}
			else
			{
				FString LoadError;
				UObject* Asset = Context.Services.LoadAsset(AssetPath, LoadError);
				UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
				if (!StaticMesh)
				{
					RowFailures.Add(TEXT("asset_not_static_mesh"));
					if (!LoadError.IsEmpty())
					{
						Row->SetStringField(TEXT("load_error"), LoadError);
					}
				}
				else
				{
					++LoadedMeshes;
					const TSharedRef<FJsonObject> Mesh = StaticMeshSummary(StaticMesh);
					Row->SetObjectField(TEXT("module_profile"), Mesh);
					const bool bHasCollision = Mesh->GetBoolField(TEXT("has_simple_collision"));
					const bool bHasSockets = SocketNames(StaticMesh).Num() > 0;
					if (bHasCollision)
					{
						++CollisionReady;
					}
					else
					{
						RowFailures.Add(TEXT("missing_simple_collision"));
					}
					if (bHasSockets)
					{
						++SocketReady;
					}
					else
					{
						RowFailures.Add(TEXT("missing_sockets"));
					}
					if (StaticMesh->GetStaticMaterials().IsEmpty())
					{
						RowFailures.Add(TEXT("missing_material_slots"));
					}
				}
			}
			for (const FString& Failure : RowFailures)
			{
				Failures.Add(FString::Printf(TEXT("module_%03d:%s"), Index + 1, *Failure));
			}
			Row->SetArrayField(TEXT("failures"), StringArrayJson(RowFailures));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}

		const bool bOk = Failures.IsEmpty() && Modules->Num() > 0;
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("kit_manifest_valid") : TEXT("kit_manifest_invalid"));
		Out->SetNumberField(TEXT("module_count"), Modules->Num());
		Out->SetNumberField(TEXT("loaded_mesh_count"), LoadedMeshes);
		Out->SetNumberField(TEXT("collision_ready_count"), CollisionReady);
		Out->SetNumberField(TEXT("socket_ready_count"), SocketReady);
		Out->SetNumberField(TEXT("kind_count"), Kinds.Num());
		Out->SetArrayField(TEXT("module_results"), Rows);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%d:%d:%d"), Modules->Num(), LoadedMeshes, CollisionReady, Failures.Num())));
		Summary = FString::Printf(TEXT("architecture_kit_manifest_validate checked %d module descriptors."), Modules->Num());
		if (!bOk)
		{
			Error = TEXT("Architecture kit manifest validation failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunArchitectureReferenceInputClassify(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_reference_input_classify"));
		FString Prompt;
		ReadStringFieldAny(Arguments, { TEXT("prompt"), TEXT("text"), TEXT("user_input") }, Prompt);
		Prompt = Prompt.TrimStartAndEnd();

		const TArray<TSharedPtr<FJsonValue>>* Attachments = nullptr;
		if (!Arguments->TryGetArrayField(TEXT("attachments"), Attachments))
		{
			Arguments->TryGetArrayField(TEXT("files"), Attachments);
		}

		int32 ImageCount = 0;
		int32 DocumentCount = 0;
		if (Attachments)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Attachments)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Kind;
				FString PathValue;
				ReadStringFieldAny(Item, { TEXT("kind"), TEXT("type"), TEXT("mime") }, Kind);
				ReadStringFieldAny(Item, { TEXT("path"), TEXT("file"), TEXT("name"), TEXT("file_name") }, PathValue);
				const FString Probe = FString::Printf(TEXT("%s %s"), *Kind.ToLower(), *PathValue.ToLower());
				if (Probe.Contains(TEXT("image")) || Probe.EndsWith(TEXT(".png")) || Probe.EndsWith(TEXT(".jpg")) || Probe.EndsWith(TEXT(".jpeg")) || Probe.EndsWith(TEXT(".webp")) || Probe.EndsWith(TEXT(".bmp")) || Probe.EndsWith(TEXT(".tga")) || Probe.EndsWith(TEXT(".exr")))
				{
					++ImageCount;
				}
				else
				{
					++DocumentCount;
				}
			}
		}

		const bool bHasText = !Prompt.IsEmpty();
		const bool bHasImages = ImageCount > 0;
		const bool bExplicitRegenerate = Prompt.Contains(TEXT("regenerate")) || Prompt.Contains(TEXT("reference"));
		FString Mode = TEXT("empty");
		if (bExplicitRegenerate && bHasText)
		{
			Mode = TEXT("explicit_regenerate_reference");
		}
		else if (bHasImages && bHasText)
		{
			Mode = TEXT("mixed_text_image");
		}
		else if (bHasImages)
		{
			Mode = TEXT("image_attached");
		}
		else if (bHasText)
		{
			Mode = TEXT("text_only");
		}

		const bool bRequiresCandidates = Mode == TEXT("text_only") || Mode == TEXT("explicit_regenerate_reference");
		const bool bUsesUserImages = Mode == TEXT("image_attached") || Mode == TEXT("mixed_text_image");
		Out->SetBoolField(TEXT("ok"), Mode != TEXT("empty"));
		Out->SetStringField(TEXT("status"), Mode == TEXT("empty") ? TEXT("reference_input_empty") : TEXT("reference_input_classified"));
		Out->SetStringField(TEXT("mode"), Mode);
		Out->SetBoolField(TEXT("has_text"), bHasText);
		Out->SetBoolField(TEXT("has_images"), bHasImages);
		Out->SetNumberField(TEXT("image_count"), ImageCount);
		Out->SetNumberField(TEXT("document_count"), DocumentCount);
		Out->SetBoolField(TEXT("requires_reference_candidates"), bRequiresCandidates);
		Out->SetBoolField(TEXT("uses_user_images_as_primary"), bUsesUserImages);
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%d:%d:%s"), *Mode, ImageCount, DocumentCount, *Prompt.Left(64))));
		Summary = FString::Printf(TEXT("architecture_reference_input_classify returned %s."), *Mode);
		if (Mode == TEXT("empty"))
		{
			Error = TEXT("Reference input is empty.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunArchitectureReferenceCandidatesGenerate(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_reference_candidates_generate"));
		FString Prompt;
		ReadStringFieldAny(Arguments, { TEXT("prompt"), TEXT("text"), TEXT("user_input") }, Prompt);
		Prompt = Prompt.TrimStartAndEnd();
		int32 CandidateCount = 4;
		double CandidateCountValue = 4.0;
		if (Arguments->TryGetNumberField(TEXT("candidate_count"), CandidateCountValue))
		{
			CandidateCount = FMath::RoundToInt(CandidateCountValue);
		}
		const bool bOk = !Prompt.IsEmpty() && CandidateCount == 4;
		TArray<TSharedPtr<FJsonValue>> Candidates;
		for (int32 Index = 0; Index < FMath::Clamp(CandidateCount, 0, 16); ++Index)
		{
			TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("id"), FString::Printf(TEXT("reference_candidate_%02d"), Index + 1));
			Candidate->SetStringField(TEXT("kind"), TEXT("image_generation_prompt"));
			Candidate->SetStringField(TEXT("prompt"), FString::Printf(TEXT("%s, modular architecture reference option %d, orthographic style board, clear facade and roof language"), *Prompt, Index + 1));
			Candidate->SetStringField(TEXT("selection_policy"), TEXT("wait_for_user_selection"));
			Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
		}
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("reference_candidates_ready_wait_user_selection") : TEXT("reference_candidates_failed_closed"));
		Out->SetNumberField(TEXT("candidate_count"), CandidateCount);
		Out->SetArrayField(TEXT("candidates"), Candidates);
		Out->SetStringField(TEXT("selection_required"), TEXT("one_or_more_reference_candidates"));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%s"), CandidateCount, *Prompt.Left(96))));
		Summary = TEXT("architecture_reference_candidates_generate produced a 4-up candidate contract.");
		if (!bOk)
		{
			Error = TEXT("Text-only reference generation requires prompt and exactly four candidates.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunArchitectureReferenceSetBuild(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_reference_set_build"));
		const TArray<TSharedPtr<FJsonValue>>* References = nullptr;
		const bool bHasReferences = Arguments->TryGetArrayField(TEXT("references"), References) || Arguments->TryGetArrayField(TEXT("images"), References) || Arguments->TryGetArrayField(TEXT("attachments"), References);
		TArray<FString> SelectedIds;
		TryGetStringArrayField(Arguments, TEXT("selected_ids"), SelectedIds);
		bool bUserImagesPrimary = false;
		Arguments->TryGetBoolField(TEXT("uses_user_images_as_primary"), bUserImagesPrimary);
		const bool bOk = (bHasReferences && References && References->Num() > 0) || SelectedIds.Num() > 0;
		TArray<TSharedPtr<FJsonValue>> SourceRows;
		if (References)
		{
			for (int32 Index = 0; Index < References->Num(); ++Index)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("id"), FString::Printf(TEXT("reference_%03d"), Index + 1));
				Row->SetStringField(TEXT("role"), bUserImagesPrimary ? TEXT("user_primary_reference") : TEXT("selected_candidate_reference"));
				SourceRows.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("reference_set_ready") : TEXT("reference_set_missing"));
		Out->SetStringField(TEXT("reference_set_id"), FString::Printf(TEXT("architecture_reference_set_%s"), *MakeReceiptHash(FString::Printf(TEXT("%d:%d"), References ? References->Num() : 0, SelectedIds.Num()))));
		Out->SetBoolField(TEXT("uses_user_images_as_primary"), bUserImagesPrimary);
		Out->SetArrayField(TEXT("selected_ids"), StringArrayJson(SelectedIds));
		Out->SetArrayField(TEXT("sources"), SourceRows);
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%d:%d"), References ? References->Num() : 0, SelectedIds.Num(), bUserImagesPrimary ? 1 : 0)));
		Summary = TEXT("architecture_reference_set_build returned a reference set contract.");
		if (!bOk)
		{
			Error = TEXT("No selected reference candidates or user image references were supplied.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunArchitectureReferenceStyleAnalyze(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_reference_style_analyze"));
		FString Prompt;
		ReadStringFieldAny(Arguments, { TEXT("prompt"), TEXT("text"), TEXT("user_input") }, Prompt);
		const FString Lower = Prompt.ToLower();
		TArray<FString> Tags;
		if (Lower.Contains(TEXT("stone")) || Lower.Contains(TEXT("fortress")) || Lower.Contains(TEXT("castle"))) { Tags.Add(TEXT("stone")); Tags.Add(TEXT("fortress")); }
		if (Lower.Contains(TEXT("wood")) || Lower.Contains(TEXT("timber"))) { Tags.Add(TEXT("wood")); }
		if (Lower.Contains(TEXT("snow")) || Lower.Contains(TEXT("nordic"))) { Tags.Add(TEXT("snow")); Tags.Add(TEXT("nordic")); }
		if (Lower.Contains(TEXT("desert"))) { Tags.Add(TEXT("desert")); }
		if (Tags.IsEmpty()) { Tags = { TEXT("wood"), TEXT("stone"), TEXT("settlement") }; }
		const FString StyleId = FString::Printf(TEXT("%s_%s_kit"), *Tags[0], Tags.Num() > 1 ? *Tags[1] : TEXT("modular"));
		TArray<FString> Palette = Tags.Contains(TEXT("desert"))
			? TArray<FString>{ TEXT("sand_plaster"), TEXT("sun_baked_stone"), TEXT("copper_trim") }
			: Tags.Contains(TEXT("snow"))
				? TArray<FString>{ TEXT("dark_wood"), TEXT("gray_stone"), TEXT("snow_cap") }
				: TArray<FString>{ TEXT("aged_wood"), TEXT("field_stone"), TEXT("lime_plaster") };
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("reference_style_analysis_ready"));
		Out->SetStringField(TEXT("style_id"), StyleId);
		Out->SetArrayField(TEXT("tags"), StringArrayJson(Tags));
		Out->SetStringField(TEXT("silhouette"), Tags.Contains(TEXT("fortress")) ? TEXT("defensive_vertical_mass") : TEXT("modular_habitation"));
		Out->SetStringField(TEXT("roof_language"), Tags.Contains(TEXT("snow")) ? TEXT("steep_snow_shedding_roof") : Tags.Contains(TEXT("desert")) ? TEXT("flat_roof_parapet") : TEXT("pitched_roof"));
		Out->SetStringField(TEXT("wall_language"), Tags.Contains(TEXT("stone")) ? TEXT("stone_base_structural_wall") : TEXT("wood_frame_infill_wall"));
		Out->SetArrayField(TEXT("material_palette"), StringArrayJson(Palette));
		Out->SetArrayField(TEXT("modularizable_elements"), StringArrayJson({ TEXT("foundation"), TEXT("wall"), TEXT("corner"), TEXT("door"), TEXT("window"), TEXT("floor"), TEXT("stair"), TEXT("roof"), TEXT("pillar"), TEXT("beam") }));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%s"), *StyleId, *Prompt.Left(96))));
		Summary = FString::Printf(TEXT("architecture_reference_style_analyze produced style %s."), *StyleId);
		return true;
	}

	static TSharedRef<FJsonObject> MakeArchitectureModuleDescriptor(const FString& Id, const FString& Kind, const TArray<double>& DimensionsCm, const TArray<FString>& SocketTypes, const FString& KitId)
	{
		TSharedRef<FJsonObject> Module = MakeShared<FJsonObject>();
		Module->SetStringField(TEXT("id"), Id);
		Module->SetStringField(TEXT("kind"), Kind);
		TArray<TSharedPtr<FJsonValue>> Dims;
		for (double Value : DimensionsCm)
		{
			Dims.Add(MakeShared<FJsonValueNumber>(Value));
		}
		Module->SetArrayField(TEXT("dimensions_cm"), Dims);
		Module->SetArrayField(TEXT("socket_types"), StringArrayJson(SocketTypes));
		Module->SetStringField(TEXT("asset_path_hint"), FString::Printf(TEXT("/Game/SOMOL/Architecture/%s/Modules/%s"), *KitId, *Id));
		Module->SetBoolField(TEXT("requires_collision"), true);
		Module->SetBoolField(TEXT("requires_lod"), true);
		Module->SetBoolField(TEXT("requires_hlod_proxy"), true);
		Module->SetBoolField(TEXT("requires_nav_clearance"), Kind == TEXT("door_wall") || Kind == TEXT("stair") || Kind == TEXT("bridge"));
		return Module;
	}

	static bool RunArchitectureKitManifestGenerate(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_kit_manifest_generate"));
		FString StyleId;
		ReadStringFieldAny(Arguments, { TEXT("style_id"), TEXT("styleId") }, StyleId);
		if (StyleId.IsEmpty()) { StyleId = TEXT("wood_stone_kit"); }
		FString KitId;
		ReadStringFieldAny(Arguments, { TEXT("kit_id"), TEXT("kitId") }, KitId);
		if (KitId.IsEmpty()) { KitId = FString::Printf(TEXT("%s_v001"), *StyleId); }
		double GridCm = 100.0;
		Arguments->TryGetNumberField(TEXT("grid_cm"), GridCm);
		GridCm = FMath::Clamp(GridCm, 10.0, 1000.0);
		double FloorHeightCm = 320.0;
		Arguments->TryGetNumberField(TEXT("floor_height_cm"), FloorHeightCm);
		FloorHeightCm = FMath::Clamp(FloorHeightCm, 180.0, 800.0);

		TArray<TSharedPtr<FJsonValue>> Modules;
		const TArray<TSharedRef<FJsonObject>> ModuleObjects = {
			MakeArchitectureModuleDescriptor(TEXT("foundation_4m"), TEXT("foundation"), { 400.0, 400.0, 40.0 }, { TEXT("floor_edge") }, KitId),
			MakeArchitectureModuleDescriptor(TEXT("wall_plain_4m"), TEXT("wall"), { 400.0, 35.0, FloorHeightCm }, { TEXT("wall_edge"), TEXT("floor_edge"), TEXT("roof_edge") }, KitId),
			MakeArchitectureModuleDescriptor(TEXT("wall_window_4m"), TEXT("wall"), { 400.0, 35.0, FloorHeightCm }, { TEXT("wall_edge"), TEXT("floor_edge"), TEXT("roof_edge") }, KitId),
			MakeArchitectureModuleDescriptor(TEXT("wall_door_4m"), TEXT("door_wall"), { 400.0, 35.0, FloorHeightCm }, { TEXT("wall_edge"), TEXT("floor_edge"), TEXT("door_anchor") }, KitId),
			MakeArchitectureModuleDescriptor(TEXT("corner_wall_90"), TEXT("corner"), { 100.0, 100.0, FloorHeightCm }, { TEXT("wall_edge"), TEXT("floor_edge"), TEXT("roof_edge") }, KitId),
			MakeArchitectureModuleDescriptor(TEXT("floor_4m"), TEXT("floor"), { 400.0, 400.0, 30.0 }, { TEXT("floor_edge"), TEXT("stair_bottom") }, KitId),
			MakeArchitectureModuleDescriptor(TEXT("stair_4m"), TEXT("stair"), { 400.0, 200.0, FloorHeightCm }, { TEXT("stair_top"), TEXT("stair_bottom") }, KitId),
			MakeArchitectureModuleDescriptor(TEXT("roof_slope_4m"), TEXT("roof"), { 400.0, 450.0, 180.0 }, { TEXT("roof_edge") }, KitId),
			MakeArchitectureModuleDescriptor(TEXT("pillar_3m"), TEXT("support"), { 40.0, 40.0, FloorHeightCm }, { TEXT("floor_edge"), TEXT("roof_edge") }, KitId),
			MakeArchitectureModuleDescriptor(TEXT("bridge_deck_8m"), TEXT("bridge"), { 800.0, 300.0, 60.0 }, { TEXT("bridge_anchor"), TEXT("road_anchor") }, KitId),
			MakeArchitectureModuleDescriptor(TEXT("tower_wall_curve_4m"), TEXT("tower_wall"), { 400.0, 80.0, FloorHeightCm }, { TEXT("wall_edge"), TEXT("floor_edge"), TEXT("roof_edge") }, KitId),
		};
		for (const TSharedRef<FJsonObject>& Module : ModuleObjects)
		{
			Modules.Add(MakeShared<FJsonValueObject>(Module));
		}
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("kit_manifest_generated"));
		Out->SetStringField(TEXT("kit_id"), KitId);
		Out->SetStringField(TEXT("style_id"), StyleId);
		Out->SetStringField(TEXT("version"), TEXT("v001"));
		Out->SetNumberField(TEXT("grid_cm"), GridCm);
		Out->SetNumberField(TEXT("floor_height_cm"), FloorHeightCm);
		Out->SetArrayField(TEXT("modules"), Modules);
		Out->SetArrayField(TEXT("module_descriptors"), Modules);
		Out->SetArrayField(TEXT("assembly_templates"), StringArrayJson({ TEXT("small_house"), TEXT("two_floor_house"), TEXT("warehouse"), TEXT("wood_bridge"), TEXT("watchtower"), TEXT("gatehouse") }));
		Out->SetArrayField(TEXT("validation_policy"), StringArrayJson({ TEXT("dimensions"), TEXT("pivot"), TEXT("orientation"), TEXT("socket_contract"), TEXT("collision"), TEXT("nav_clearance"), TEXT("lod"), TEXT("preview") }));
		Out->SetNumberField(TEXT("module_count"), Modules.Num());
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%s:%d"), *KitId, *StyleId, Modules.Num())));
		Summary = FString::Printf(TEXT("architecture_kit_manifest_generate produced %d module descriptors."), Modules.Num());
		return true;
	}

	static bool RunArchitectureModuleDagGenerate(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_module_dag_generate"));
		const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
		if (!ResolveModuleDescriptors(Arguments, Modules) || !Modules)
		{
			Error = TEXT("Missing module_descriptors or modules for module DAG generation.");
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("status"), TEXT("module_dag_missing_manifest"));
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		FString KitId;
		ReadStringFieldAny(Arguments, { TEXT("kit_id"), TEXT("kitId") }, KitId);
		if (KitId.IsEmpty()) { KitId = TEXT("architecture_kit_v001"); }
		TArray<TSharedPtr<FJsonValue>> Tasks;
		for (int32 Index = 0; Index < Modules->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Module = (*Modules)[Index].IsValid() ? (*Modules)[Index]->AsObject() : nullptr;
			FString ModuleId;
			ReadStringFieldAny(Module, { TEXT("id"), TEXT("module_id"), TEXT("name") }, ModuleId);
			if (ModuleId.IsEmpty()) { ModuleId = FString::Printf(TEXT("module_%03d"), Index + 1); }
			TSharedRef<FJsonObject> Task = MakeShared<FJsonObject>();
			Task->SetStringField(TEXT("id"), FString::Printf(TEXT("module_%03d_%s"), Index + 1, *ModuleId));
			Task->SetStringField(TEXT("kind"), TEXT("architecture_module_asset"));
			Task->SetStringField(TEXT("module_id"), ModuleId);
			Task->SetStringField(TEXT("asset_path_hint"), ReadModuleAssetPath(Module).IsEmpty() ? FString::Printf(TEXT("/Game/SOMOL/Architecture/%s/Modules/%s"), *KitId, *ModuleId) : ReadModuleAssetPath(Module));
			Task->SetArrayField(TEXT("acceptance"), StringArrayJson({ TEXT("mesh_exists"), TEXT("metadata_exists"), TEXT("dimensions_ok"), TEXT("pivot_ok"), TEXT("sockets_ok"), TEXT("collision_ok"), TEXT("lod_ok"), TEXT("preview_ok") }));
			Task->SetArrayField(TEXT("receipts"), StringArrayJson({ FString::Printf(TEXT("%s_module_asset_receipt"), *ModuleId) }));
			Tasks.Add(MakeShared<FJsonValueObject>(Task));
		}
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("module_production_dag_ready"));
		Out->SetStringField(TEXT("kit_id"), KitId);
		Out->SetNumberField(TEXT("task_count"), Tasks.Num());
		Out->SetArrayField(TEXT("tasks"), Tasks);
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%d"), *KitId, Tasks.Num())));
		Summary = FString::Printf(TEXT("architecture_module_dag_generate produced %d module tasks."), Tasks.Num());
		return true;
	}

	static bool RunArchitectureCollisionContractValidate(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_collision_contract_validate"));
		UStaticMesh* StaticMesh = LoadStaticMesh(Context, Arguments, Out, Error);
		if (!StaticMesh)
		{
			return false;
		}

		bool bRequiresSimpleCollision = true;
		Arguments->TryGetBoolField(TEXT("requires_simple_collision"), bRequiresSimpleCollision);

		double MinDoorWidthCm = 0.0;
		double MinDoorHeightCm = 0.0;
		Arguments->TryGetNumberField(TEXT("min_door_width_cm"), MinDoorWidthCm);
		Arguments->TryGetNumberField(TEXT("min_door_height_cm"), MinDoorHeightCm);

		const FBoxSphereBounds Bounds = StaticMesh->GetBounds();
		const UBodySetup* BodySetup = StaticMesh->GetBodySetup();
		const int32 SimpleGeomCount = CountSimpleGeoms(BodySetup);
		TArray<FString> Failures;
		if (bRequiresSimpleCollision && SimpleGeomCount <= 0)
		{
			Failures.Add(TEXT("missing_simple_collision"));
		}
		if (MinDoorWidthCm > 0.0 && Bounds.BoxExtent.X * 2.0 < MinDoorWidthCm)
		{
			Failures.Add(TEXT("door_width_below_contract"));
		}
		if (MinDoorHeightCm > 0.0 && Bounds.BoxExtent.Z * 2.0 < MinDoorHeightCm)
		{
			Failures.Add(TEXT("door_height_below_contract"));
		}

		const bool bOk = Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("collision_contract_valid") : TEXT("collision_contract_failed"));
		Out->SetObjectField(TEXT("mesh_profile"), StaticMeshSummary(StaticMesh));
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		if (!bOk)
		{
			Error = TEXT("Architecture collision contract failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		Summary = FString::Printf(TEXT("Architecture collision contract valid for %s."), *StaticMesh->GetName());
		return true;
	}

	static TSharedRef<FJsonObject> GenericArchitectureInputSchema(bool bMutating)
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Return receipt/plan only. Defaults true for mutating architecture tools."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Attempt editor mutation when supported. Mutating tools require explicit execute=true."))},
			{TEXT("assembly_id"), FSololmcpSchemaBuilder::String(TEXT("Optional deterministic assembly id used for actor tags/readback."))},
			{TEXT("actor_labels"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Actor label/name/path")), TEXT("Actors to read back or audit."))},
			{TEXT("modules"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Shortcut for assembly_plan.modules."))},
			{TEXT("architecture_recipe"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("ArchitectureRecipe payload."))},
			{TEXT("kit_manifest"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("ModularKitManifest payload."))},
			{TEXT("module_descriptors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Shortcut for kit_manifest.module_descriptors."))},
			{TEXT("assembly_plan"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Assembly plan payload."))},
			{TEXT("grid_unit_cm"), FSololmcpSchemaBuilder::Number(TEXT("Modular snap grid in centimeters."))},
			{TEXT("floor_height_cm"), FSololmcpSchemaBuilder::Number(TEXT("Floor height in centimeters."))},
			{TEXT("max_buildings_per_plan"), FSololmcpSchemaBuilder::Integer(TEXT("Guard for settlement to assembly conversion."))},
			{TEXT("include_interior_stairs"), FSololmcpSchemaBuilder::Boolean(TEXT("Add stair modules for multi-floor buildings when a stair kind exists."))},
			{TEXT("collision_contract"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("CollisionContract payload."))},
			{TEXT("interior_nav_graph"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("InteriorNavGraph payload."))},
			{TEXT("settlement_plan"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Settlement/district plan payload."))},
			{TEXT("settlement_id"), FSololmcpSchemaBuilder::String(TEXT("Deterministic settlement id for generated layout contracts."))},
			{TEXT("style_id"), FSololmcpSchemaBuilder::String(TEXT("Architecture style id applied to generated district/building plans."))},
			{TEXT("center_cm"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Settlement center in centimeters: x/y/z."))},
			{TEXT("radius_cm"), FSololmcpSchemaBuilder::Number(TEXT("Settlement planning radius in centimeters."))},
			{TEXT("district_count"), FSololmcpSchemaBuilder::Integer(TEXT("Number of settlement districts to plan."))},
			{TEXT("building_count"), FSololmcpSchemaBuilder::Integer(TEXT("Total number of building footprints to plan; supports arbitrary city scale through paging."))},
			{TEXT("target_building_count"), FSololmcpSchemaBuilder::Integer(TEXT("Alias for total building_count."))},
			{TEXT("building_offset"), FSololmcpSchemaBuilder::Integer(TEXT("Zero-based building page offset. Default 0."))},
			{TEXT("building_page_size"), FSololmcpSchemaBuilder::Integer(TEXT("Buildings emitted by this call. Default/max 512; use next_building_offset for subsequent pages."))},
			{TEXT("road_ring_count"), FSololmcpSchemaBuilder::Integer(TEXT("Number of conceptual road rings."))},
			{TEXT("district_types"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("District type")), TEXT("Ordered district type palette."))},
			{TEXT("building_archetypes"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Building archetype")), TEXT("Ordered building archetype palette."))},
			{TEXT("bridge_plan"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Bridge plan with deck width, slope, endpoints, road nodes, and nav links."))},
			{TEXT("fortress_plan"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Fortress plan with gates, navigation nodes, and nav links."))},
			{TEXT("endpoints"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Shortcut for bridge_plan.endpoints."))},
			{TEXT("road_nodes"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Shortcut for bridge_plan.road_nodes."))},
			{TEXT("gates"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Shortcut for fortress_plan.gates."))},
			{TEXT("nodes"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Shortcut for fortress_plan.nodes."))},
			{TEXT("nav_links"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Bridge or fortress navigation links."))},
			{TEXT("require_open_path"), FSololmcpSchemaBuilder::Boolean(TEXT("Fortress validation requires at least one open connected gate. Defaults true."))},
			{TEXT("allow_closed_gate"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow closed fortress gates as intentional design states."))},
			{TEXT("require_passable_when_closed"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow a closed drawbridge to remain passable by contract."))},
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Prior receipt to validate or continue."))}
		});
	}

	static bool ResolveAssemblyPlanAndModules(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedPtr<FJsonObject>& OutPlan,
		const TArray<TSharedPtr<FJsonValue>>*& OutModules)
	{
		OutPlan = Arguments;
		if (const TSharedPtr<FJsonObject>* PlanPtr = nullptr; Arguments->TryGetObjectField(TEXT("assembly_plan"), PlanPtr) && PlanPtr && PlanPtr->IsValid())
		{
			OutPlan = *PlanPtr;
		}
		OutModules = nullptr;
		if (OutPlan.IsValid() && OutPlan->TryGetArrayField(TEXT("modules"), OutModules) && OutModules)
		{
			return true;
		}
		if (Arguments->TryGetArrayField(TEXT("modules"), OutModules) && OutModules)
		{
			return true;
		}
		return false;
	}

	static bool RunArchitectureAssemblyExecute(
		const FSololmcpToolExecutionContext& Context,
		const TArray<FString>& RequiredReceipts,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_assembly_execute"));
		Out->SetArrayField(TEXT("required_receipts"), StringArrayJson(RequiredReceipts));
		Out->SetBoolField(TEXT("mutating"), true);

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("architecture_assembly_writer_ready_dry_run"));
			Summary = TEXT("architecture_assembly_execute dry-run contract returned; pass execute=true to spawn modules.");
			return true;
		}

		TSharedPtr<FJsonObject> Plan;
		const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
		if (!ResolveAssemblyPlanAndModules(Arguments, Plan, Modules) || !Modules || Modules->IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("assembly_plan.modules"));
			Error = TEXT("Missing assembly_plan.modules or modules.");
			return false;
		}

		int32 MaxModules = 128;
		Arguments->TryGetNumberField(TEXT("max_modules"), MaxModules);
		if (Modules->Num() > FMath::Clamp(MaxModules, 1, 512))
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error_code"), TEXT("MODULE_LIMIT_EXCEEDED"));
			Error = FString::Printf(TEXT("Assembly module count %d exceeds max_modules %d."), Modules->Num(), MaxModules);
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		UWorld* World = GetEditorWorld(Context, Out, Error);
		if (!World)
		{
			return false;
		}

		FString AssemblyId;
		Arguments->TryGetStringField(TEXT("assembly_id"), AssemblyId);
		if (AssemblyId.IsEmpty() && Plan.IsValid())
		{
			Plan->TryGetStringField(TEXT("assembly_id"), AssemblyId);
		}
		if (AssemblyId.IsEmpty())
		{
			AssemblyId = FString::Printf(TEXT("assembly_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		}
		const FName AssemblyTag(*FString::Printf(TEXT("SOMOLArchitectureAssembly:%s"), *AssemblyId));

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ArchitectureAssemblyExecute", "SOMOLMCP Architecture Assembly Execute"));
		TArray<TSharedPtr<FJsonValue>> SpawnedActors;
		TArray<FString> Failures;
		int32 SpawnedCount = 0;
		int32 CollisionEnabledCount = 0;
		int32 NavRelevantCount = 0;

		for (int32 Index = 0; Index < Modules->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Module = (*Modules)[Index].IsValid() ? (*Modules)[Index]->AsObject() : nullptr;
			if (!Module.IsValid())
			{
				Failures.Add(FString::Printf(TEXT("module[%d] is not an object"), Index));
				continue;
			}

			FString AssetPath;
			if (!Module->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
			{
				Failures.Add(FString::Printf(TEXT("module[%d] missing asset_path"), Index));
				continue;
			}
			UStaticMesh* StaticMesh = LoadStaticMeshByPath(Context, AssetPath, Out, Error);
			if (!StaticMesh)
			{
				Failures.Add(FString::Printf(TEXT("module[%d] failed to load %s: %s"), Index, *AssetPath, *Error));
				continue;
			}

			FString ModuleId;
			Module->TryGetStringField(TEXT("module_id"), ModuleId);
			if (ModuleId.IsEmpty())
			{
				ModuleId = FString::Printf(TEXT("module_%03d"), Index);
			}
			FString Kind;
			Module->TryGetStringField(TEXT("kind"), Kind);

			FVector Location = FVector::ZeroVector;
			TryReadVectorField(Module, TEXT("location_cm"), Location);
			TryReadVectorField(Module, TEXT("location"), Location);
			FRotator Rotation = FRotator::ZeroRotator;
			TryReadRotatorField(Module, TEXT("rotation_deg"), Rotation);
			TryReadRotatorField(Module, TEXT("rotation"), Rotation);
			FVector Scale = FVector::OneVector;
			TryReadScaleField(Module, TEXT("scale"), Scale);
			TryReadScaleField(Module, TEXT("scale_xyz"), Scale);

			FString DesiredLabel;
			if (!Module->TryGetStringField(TEXT("actor_label"), DesiredLabel) || DesiredLabel.TrimStartAndEnd().IsEmpty())
			{
				DesiredLabel = FString::Printf(TEXT("SOMOL_%s_%s"), *AssemblyId, *ModuleId);
			}
			DesiredLabel = MakeUniqueActorLabel(World, DesiredLabel);

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParams.bAllowDuringConstructionScript = true;
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(Rotation, Location, Scale), SpawnParams);
			if (!Actor || !Actor->GetStaticMeshComponent())
			{
				Failures.Add(FString::Printf(TEXT("module[%d] SpawnActor failed"), Index));
				continue;
			}

			Actor->Modify();
			Actor->SetActorLabel(DesiredLabel);
			Actor->Tags.AddUnique(AssemblyTag);
			Actor->Tags.AddUnique(FName(TEXT("SOMOLArchitectureModule")));
			if (!Kind.IsEmpty())
			{
				Actor->Tags.AddUnique(FName(*FString::Printf(TEXT("SOMOLArchitectureKind:%s"), *Kind)));
			}

			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->Modify();
			Component->SetStaticMesh(StaticMesh);
			Component->SetMobility(EComponentMobility::Static);
			Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Component->SetCollisionProfileName(TEXT("BlockAll"));
			Component->SetCanEverAffectNavigation(true);
			Component->MarkRenderStateDirty();
			Actor->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(Actor);

			if (Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
			{
				++CollisionEnabledCount;
			}
			if (Component->CanEverAffectNavigation())
			{
				++NavRelevantCount;
			}
			++SpawnedCount;
			SpawnedActors.Add(MakeShared<FJsonValueObject>(ActorReadbackJson(Actor)));
		}

		const bool bOk = SpawnedCount == Modules->Num() && Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("assembly_executed") : TEXT("assembly_partial_or_failed"));
		Out->SetStringField(TEXT("assembly_id"), AssemblyId);
		Out->SetNumberField(TEXT("requested_module_count"), Modules->Num());
		Out->SetNumberField(TEXT("spawned_count"), SpawnedCount);
		Out->SetNumberField(TEXT("collision_enabled_component_count"), CollisionEnabledCount);
		Out->SetNumberField(TEXT("nav_relevant_component_count"), NavRelevantCount);
		Out->SetArrayField(TEXT("spawned_actors"), SpawnedActors);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%d:%d:%d"), *AssemblyId, Modules->Num(), SpawnedCount, Failures.Num())));

		Summary = FString::Printf(TEXT("architecture_assembly_execute spawned %d/%d module actors for %s."), SpawnedCount, Modules->Num(), *AssemblyId);
		if (!bOk)
		{
			Error = TEXT("Architecture assembly finished with missing modules or failures.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunArchitectureAssemblyReadback(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_assembly_readback"));
		UWorld* World = GetEditorWorld(Context, Out, Error);
		if (!World)
		{
			return false;
		}

		TArray<AActor*> Actors;
		FString AssemblyId;
		ResolveArchitectureActors(World, Arguments, Actors, AssemblyId);
		TArray<TSharedPtr<FJsonValue>> Readback;
		int32 ComponentCount = 0;
		int32 CollisionEnabledCount = 0;
		for (AActor* Actor : Actors)
		{
			TSharedRef<FJsonObject> ActorJson = ActorReadbackJson(Actor);
			ComponentCount += static_cast<int32>(ActorJson->GetNumberField(TEXT("static_mesh_component_count")));
			CollisionEnabledCount += static_cast<int32>(ActorJson->GetNumberField(TEXT("collision_enabled_component_count")));
			Readback.Add(MakeShared<FJsonValueObject>(ActorJson));
		}

		const bool bOk = Actors.Num() > 0;
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("assembly_readback_ok") : TEXT("assembly_readback_empty"));
		Out->SetStringField(TEXT("assembly_id"), AssemblyId);
		Out->SetNumberField(TEXT("actor_count"), Actors.Num());
		Out->SetNumberField(TEXT("static_mesh_component_count"), ComponentCount);
		Out->SetNumberField(TEXT("collision_enabled_component_count"), CollisionEnabledCount);
		Out->SetArrayField(TEXT("actors"), Readback);
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%d:%d:%d"), *AssemblyId, Actors.Num(), ComponentCount, CollisionEnabledCount)));
		Summary = FString::Printf(TEXT("architecture_assembly_readback found %d actors."), Actors.Num());
		if (!bOk)
		{
			Error = TEXT("No architecture actors matched actor_labels, receipt, or assembly_id.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunArchitectureCollisionGenerate(
		const FSololmcpToolExecutionContext& Context,
		const TArray<FString>& RequiredReceipts,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_collision_generate"));
		Out->SetArrayField(TEXT("required_receipts"), StringArrayJson(RequiredReceipts));
		Out->SetBoolField(TEXT("mutating"), true);
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("architecture_collision_writer_ready_dry_run"));
			Summary = TEXT("architecture_collision_generate dry-run contract returned; pass execute=true to update components.");
			return true;
		}

		UWorld* World = GetEditorWorld(Context, Out, Error);
		if (!World)
		{
			return false;
		}
		TArray<AActor*> Actors;
		FString AssemblyId;
		ResolveArchitectureActors(World, Arguments, Actors, AssemblyId);
		if (Actors.IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("status"), TEXT("collision_generate_no_actors"));
			Error = TEXT("No architecture actors matched actor_labels, receipt, or assembly_id.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		FString CollisionProfile = TEXT("BlockAll");
		Arguments->TryGetStringField(TEXT("collision_profile"), CollisionProfile);
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ArchitectureCollisionGenerate", "SOMOLMCP Architecture Collision Generate"));
		int32 TouchedComponents = 0;
		int32 CollisionEnabledCount = 0;
		int32 NavRelevantCount = 0;
		for (AActor* Actor : Actors)
		{
			if (!IsValid(Actor))
			{
				continue;
			}
			Actor->Modify();
			TArray<UStaticMeshComponent*> Components;
			Actor->GetComponents<UStaticMeshComponent>(Components);
			for (UStaticMeshComponent* Component : Components)
			{
				if (!Component)
				{
					continue;
				}
				Component->Modify();
				Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				Component->SetCollisionProfileName(FName(*CollisionProfile));
				Component->SetCanEverAffectNavigation(true);
				Component->MarkRenderStateDirty();
				++TouchedComponents;
				if (Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
				{
					++CollisionEnabledCount;
				}
				if (Component->CanEverAffectNavigation())
				{
					++NavRelevantCount;
				}
			}
			Actor->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(Actor);
		}

		const bool bOk = TouchedComponents > 0 && TouchedComponents == CollisionEnabledCount;
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("collision_components_enabled") : TEXT("collision_components_partial"));
		Out->SetStringField(TEXT("assembly_id"), AssemblyId);
		Out->SetNumberField(TEXT("actor_count"), Actors.Num());
		Out->SetNumberField(TEXT("touched_component_count"), TouchedComponents);
		Out->SetNumberField(TEXT("collision_enabled_component_count"), CollisionEnabledCount);
		Out->SetNumberField(TEXT("nav_relevant_component_count"), NavRelevantCount);
		Out->SetStringField(TEXT("collision_profile"), CollisionProfile);
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%d:%d:%d"), *AssemblyId, TouchedComponents, CollisionEnabledCount, NavRelevantCount)));
		Summary = FString::Printf(TEXT("architecture_collision_generate enabled collision on %d/%d components."), CollisionEnabledCount, TouchedComponents);
		if (!bOk)
		{
			Error = TEXT("Collision generation touched no components or did not fully enable collision.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunArchitectureCollisionAudit(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_collision_audit"));
		UWorld* World = GetEditorWorld(Context, Out, Error);
		if (!World)
		{
			return false;
		}

		TArray<AActor*> Actors;
		FString AssemblyId;
		ResolveArchitectureActors(World, Arguments, Actors, AssemblyId);
		TArray<TSharedPtr<FJsonValue>> ActorReports;
		int32 ComponentCount = 0;
		int32 CollisionEnabledCount = 0;
		int32 NavRelevantCount = 0;
		int32 SimpleGeomCount = 0;
		for (AActor* Actor : Actors)
		{
			TSharedRef<FJsonObject> Report = ActorReadbackJson(Actor);
			ComponentCount += static_cast<int32>(Report->GetNumberField(TEXT("static_mesh_component_count")));
			CollisionEnabledCount += static_cast<int32>(Report->GetNumberField(TEXT("collision_enabled_component_count")));
			NavRelevantCount += static_cast<int32>(Report->GetNumberField(TEXT("nav_relevant_component_count")));
			SimpleGeomCount += static_cast<int32>(Report->GetNumberField(TEXT("simple_collision_geom_count")));
			ActorReports.Add(MakeShared<FJsonValueObject>(Report));
		}

		TArray<FString> Failures;
		if (Actors.IsEmpty())
		{
			Failures.Add(TEXT("no_matching_actors"));
		}
		if (ComponentCount <= 0)
		{
			Failures.Add(TEXT("no_static_mesh_components"));
		}
		if (CollisionEnabledCount < ComponentCount)
		{
			Failures.Add(TEXT("collision_disabled_components"));
		}
		if (SimpleGeomCount <= 0)
		{
			Failures.Add(TEXT("no_simple_collision_geometry_on_mesh_assets"));
		}

		const bool bOk = Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("collision_audit_ok") : TEXT("collision_audit_failed"));
		Out->SetStringField(TEXT("assembly_id"), AssemblyId);
		Out->SetNumberField(TEXT("actor_count"), Actors.Num());
		Out->SetNumberField(TEXT("static_mesh_component_count"), ComponentCount);
		Out->SetNumberField(TEXT("collision_enabled_component_count"), CollisionEnabledCount);
		Out->SetNumberField(TEXT("nav_relevant_component_count"), NavRelevantCount);
		Out->SetNumberField(TEXT("simple_collision_geom_count"), SimpleGeomCount);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetArrayField(TEXT("actors"), ActorReports);
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%d:%d:%d:%d"), *AssemblyId, Actors.Num(), ComponentCount, CollisionEnabledCount, SimpleGeomCount)));
		Summary = FString::Printf(TEXT("architecture_collision_audit actors=%d components=%d collision=%d simple_geoms=%d."), Actors.Num(), ComponentCount, CollisionEnabledCount, SimpleGeomCount);
		if (!bOk)
		{
			Error = TEXT("Architecture collision audit failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunArchitectureReachabilityAudit(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_reachability_audit"));

		TSharedPtr<FJsonObject> Graph = Arguments;
		if (const TSharedPtr<FJsonObject>* GraphPtr = nullptr; Arguments->TryGetObjectField(TEXT("interior_nav_graph"), GraphPtr) && GraphPtr && GraphPtr->IsValid())
		{
			Graph = *GraphPtr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
		const bool bHasGraph = Graph.IsValid()
			&& Graph->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes
			&& Graph->TryGetArrayField(TEXT("links"), Links) && Links;

		TSet<FString> NodeIds;
		TMap<FString, TArray<FString>> Edges;
		FString StartNode;
		int32 EntranceNodeCount = 0;
		if (bHasGraph)
		{
			for (int32 Index = 0; Index < Nodes->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Node = (*Nodes)[Index].IsValid() ? (*Nodes)[Index]->AsObject() : nullptr;
				if (!Node.IsValid())
				{
					continue;
				}
				FString Id;
				if (!Node->TryGetStringField(TEXT("id"), Id))
				{
					Node->TryGetStringField(TEXT("node_id"), Id);
				}
				if (Id.TrimStartAndEnd().IsEmpty())
				{
					Id = FString::Printf(TEXT("node_%03d"), Index);
				}
				NodeIds.Add(Id);
				FString Kind;
				Node->TryGetStringField(TEXT("kind"), Kind);
				if (Kind.IsEmpty())
				{
					Node->TryGetStringField(TEXT("node_kind"), Kind);
				}
				if (Kind.Equals(TEXT("entrance"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("door"), ESearchCase::IgnoreCase))
				{
					++EntranceNodeCount;
					if (StartNode.IsEmpty())
					{
						StartNode = Id;
					}
				}
				if (StartNode.IsEmpty())
				{
					StartNode = Id;
				}
			}

			for (const TSharedPtr<FJsonValue>& LinkValue : *Links)
			{
				const TSharedPtr<FJsonObject> Link = LinkValue.IsValid() ? LinkValue->AsObject() : nullptr;
				if (!Link.IsValid())
				{
					continue;
				}
				FString From;
				FString To;
				if (!Link->TryGetStringField(TEXT("from"), From))
				{
					Link->TryGetStringField(TEXT("from_id"), From);
				}
				if (!Link->TryGetStringField(TEXT("to"), To))
				{
					Link->TryGetStringField(TEXT("to_id"), To);
				}
				if (!From.IsEmpty() && !To.IsEmpty())
				{
					Edges.FindOrAdd(From).Add(To);
					Edges.FindOrAdd(To).Add(From);
				}
			}
		}

		TSet<FString> Visited;
		if (!StartNode.IsEmpty())
		{
			TArray<FString> Stack;
			Stack.Add(StartNode);
			while (!Stack.IsEmpty())
			{
				const FString Current = Stack.Pop(SOMOLMCP_NO_SHRINK);
				if (Visited.Contains(Current))
				{
					continue;
				}
				Visited.Add(Current);
				if (const TArray<FString>* Neighbors = Edges.Find(Current))
				{
					for (const FString& Neighbor : *Neighbors)
					{
						if (!Visited.Contains(Neighbor))
						{
							Stack.Add(Neighbor);
						}
					}
				}
			}
		}

		int32 ActorCount = 0;
		int32 ComponentCount = 0;
		int32 CollisionEnabledCount = 0;
		FString WorldError;
		if (UWorld* World = Context.Services.GetEditorWorld(WorldError))
		{
			TArray<AActor*> Actors;
			FString AssemblyId;
			ResolveArchitectureActors(World, Arguments, Actors, AssemblyId);
			ActorCount = Actors.Num();
			for (AActor* Actor : Actors)
			{
				if (!IsValid(Actor))
				{
					continue;
				}
				TArray<UStaticMeshComponent*> Components;
				Actor->GetComponents<UStaticMeshComponent>(Components);
				ComponentCount += Components.Num();
				for (UStaticMeshComponent* Component : Components)
				{
					if (Component && Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
					{
						++CollisionEnabledCount;
					}
				}
			}
		}

		TArray<FString> Failures;
		if (bHasGraph)
		{
			if (NodeIds.IsEmpty())
			{
				Failures.Add(TEXT("interior_nav_graph_empty"));
			}
			if (EntranceNodeCount <= 0)
			{
				Failures.Add(TEXT("missing_entrance_node"));
			}
			if (Visited.Num() != NodeIds.Num())
			{
				Failures.Add(TEXT("interior_nav_graph_disconnected"));
			}
		}
		else if (ActorCount <= 0)
		{
			Failures.Add(TEXT("missing_interior_nav_graph_or_assembly_readback"));
		}
		if (ActorCount > 0 && ComponentCount > 0 && CollisionEnabledCount < ComponentCount)
		{
			Failures.Add(TEXT("assembly_collision_not_fully_enabled"));
		}

		const bool bOk = Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("reachability_contract_ok") : TEXT("reachability_contract_failed"));
		Out->SetNumberField(TEXT("interior_node_count"), NodeIds.Num());
		Out->SetNumberField(TEXT("interior_link_count"), bHasGraph ? Links->Num() : 0);
		Out->SetNumberField(TEXT("entrance_node_count"), EntranceNodeCount);
		Out->SetNumberField(TEXT("connected_node_count"), Visited.Num());
		Out->SetNumberField(TEXT("actor_count"), ActorCount);
		Out->SetNumberField(TEXT("static_mesh_component_count"), ComponentCount);
		Out->SetNumberField(TEXT("collision_enabled_component_count"), CollisionEnabledCount);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%d:%d:%d:%d"), NodeIds.Num(), Links ? Links->Num() : 0, Visited.Num(), ActorCount, CollisionEnabledCount)));
		Summary = FString::Printf(TEXT("architecture_reachability_audit nodes=%d connected=%d actors=%d."), NodeIds.Num(), Visited.Num(), ActorCount);
		if (!bOk)
		{
			Error = TEXT("Architecture reachability contract failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunSettlementRoadToEntranceValidate(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("settlement_road_to_entrance_validate"));
		TSharedPtr<FJsonObject> Plan = Arguments;
		if (const TSharedPtr<FJsonObject>* PlanPtr = nullptr; Arguments->TryGetObjectField(TEXT("settlement_plan"), PlanPtr) && PlanPtr && PlanPtr->IsValid())
		{
			Plan = *PlanPtr;
		}

		const TArray<TSharedPtr<FJsonValue>>* RoadNodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Entrances = nullptr;
		if (!Plan.IsValid() || !Plan->TryGetArrayField(TEXT("road_nodes"), RoadNodes) || !RoadNodes)
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("settlement_plan.road_nodes"));
			Error = TEXT("Missing settlement_plan.road_nodes.");
			return false;
		}
		if (!Plan->TryGetArrayField(TEXT("entrances"), Entrances) || !Entrances)
		{
			Plan->TryGetArrayField(TEXT("entrance_nodes"), Entrances);
		}
		if (!Entrances)
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("settlement_plan.entrances"));
			Error = TEXT("Missing settlement_plan.entrances or entrance_nodes.");
			return false;
		}

		double MaxDistanceCm = 600.0;
		Arguments->TryGetNumberField(TEXT("max_connection_distance_cm"), MaxDistanceCm);
		Plan->TryGetNumberField(TEXT("max_connection_distance_cm"), MaxDistanceCm);

		TSet<FString> RoadIds;
		TArray<FVector> RoadLocations;
		for (int32 Index = 0; Index < RoadNodes->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Road = (*RoadNodes)[Index].IsValid() ? (*RoadNodes)[Index]->AsObject() : nullptr;
			if (!Road.IsValid())
			{
				continue;
			}
			FString Id;
			if (!Road->TryGetStringField(TEXT("id"), Id))
			{
				Road->TryGetStringField(TEXT("node_id"), Id);
			}
			if (Id.IsEmpty())
			{
				Id = FString::Printf(TEXT("road_%03d"), Index);
			}
			RoadIds.Add(Id);
			FVector Location = FVector::ZeroVector;
			if (TryReadVectorField(Road, TEXT("location_cm"), Location) || TryReadVectorField(Road, TEXT("location"), Location))
			{
				RoadLocations.Add(Location);
			}
		}

		TArray<TSharedPtr<FJsonValue>> EntranceReports;
		TArray<FString> Failures;
		int32 ConnectedCount = 0;
		for (int32 Index = 0; Index < Entrances->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Entrance = (*Entrances)[Index].IsValid() ? (*Entrances)[Index]->AsObject() : nullptr;
			TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
			Report->SetNumberField(TEXT("index"), Index);
			if (!Entrance.IsValid())
			{
				Report->SetBoolField(TEXT("connected"), false);
				Report->SetStringField(TEXT("failure"), TEXT("entrance_not_object"));
				Failures.Add(FString::Printf(TEXT("entrance[%d]_not_object"), Index));
				EntranceReports.Add(MakeShared<FJsonValueObject>(Report));
				continue;
			}

			FString EntranceId;
			if (!Entrance->TryGetStringField(TEXT("id"), EntranceId))
			{
				Entrance->TryGetStringField(TEXT("entrance_id"), EntranceId);
			}
			if (EntranceId.IsEmpty())
			{
				EntranceId = FString::Printf(TEXT("entrance_%03d"), Index);
			}
			Report->SetStringField(TEXT("entrance_id"), EntranceId);

			FString ConnectedRoadId;
			Entrance->TryGetStringField(TEXT("connected_road_id"), ConnectedRoadId);
			bool bConnected = !ConnectedRoadId.IsEmpty() && RoadIds.Contains(ConnectedRoadId);
			double NearestDistance = TNumericLimits<double>::Max();
			FVector EntranceLocation = FVector::ZeroVector;
			if (!bConnected && (TryReadVectorField(Entrance, TEXT("location_cm"), EntranceLocation) || TryReadVectorField(Entrance, TEXT("location"), EntranceLocation)))
			{
				for (const FVector& RoadLocation : RoadLocations)
				{
					NearestDistance = FMath::Min(NearestDistance, FVector::Distance(EntranceLocation, RoadLocation));
				}
				bConnected = NearestDistance <= MaxDistanceCm;
			}

			Report->SetBoolField(TEXT("connected"), bConnected);
			Report->SetStringField(TEXT("connected_road_id"), ConnectedRoadId);
			Report->SetNumberField(TEXT("nearest_road_distance_cm"), NearestDistance == TNumericLimits<double>::Max() ? -1.0 : NearestDistance);
			if (bConnected)
			{
				++ConnectedCount;
			}
			else
			{
				const FString Failure = FString::Printf(TEXT("entrance[%d]_%s_not_connected_to_road"), Index, *EntranceId);
				Report->SetStringField(TEXT("failure"), Failure);
				Failures.Add(Failure);
			}
			EntranceReports.Add(MakeShared<FJsonValueObject>(Report));
		}

		const bool bOk = Entrances->Num() > 0 && Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("road_to_entrance_valid") : TEXT("road_to_entrance_failed"));
		Out->SetNumberField(TEXT("road_node_count"), RoadIds.Num());
		Out->SetNumberField(TEXT("entrance_count"), Entrances->Num());
		Out->SetNumberField(TEXT("connected_entrance_count"), ConnectedCount);
		Out->SetNumberField(TEXT("max_connection_distance_cm"), MaxDistanceCm);
		Out->SetArrayField(TEXT("entrances"), EntranceReports);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%d:%d:%d"), RoadIds.Num(), Entrances->Num(), ConnectedCount, Failures.Num())));
		Summary = FString::Printf(TEXT("settlement_road_to_entrance_validate connected %d/%d entrances."), ConnectedCount, Entrances->Num());
		if (!bOk)
		{
			Error = TEXT("Settlement road-to-entrance validation failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> ResolveNamedPlanObject(
		const TSharedRef<FJsonObject>& Arguments,
		const TCHAR* FieldName)
	{
		if (const TSharedPtr<FJsonObject>* PlanPtr = nullptr; Arguments->TryGetObjectField(FieldName, PlanPtr) && PlanPtr && PlanPtr->IsValid())
		{
			return *PlanPtr;
		}
		return Arguments;
	}

	static bool ReadStringFieldAny(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, FString& Out)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		for (const FString& FieldName : FieldNames)
		{
			if (Object->TryGetStringField(FieldName, Out) && !Out.TrimStartAndEnd().IsEmpty())
			{
				Out.TrimStartAndEndInline();
				return true;
			}
		}
		return false;
	}

	static bool ReadNumberFieldAny(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, double& Out)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		for (const FString& FieldName : FieldNames)
		{
			if (Object->TryGetNumberField(FieldName, Out))
			{
				return true;
			}
		}
		return false;
	}

	static bool ReadStringArrayFieldAny(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, TArray<FString>& Out)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		for (const FString& FieldName : FieldNames)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object->TryGetArrayField(FieldName, Values) || !Values)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (!Value.IsValid())
				{
					continue;
				}
				FString Text = Value->AsString().TrimStartAndEnd();
				if (!Text.IsEmpty())
				{
					Out.Add(Text);
				}
			}
			return !Out.IsEmpty();
		}
		return false;
	}

	static FString MakeArchitectureIdToken(FString Value, const FString& Fallback)
	{
		Value.TrimStartAndEndInline();
		if (Value.IsEmpty())
		{
			Value = Fallback;
		}
		Value.ReplaceInline(TEXT(" "), TEXT("_"));
		Value.ReplaceInline(TEXT("/"), TEXT("_"));
		Value.ReplaceInline(TEXT("\\"), TEXT("_"));
		Value.ReplaceInline(TEXT("."), TEXT("_"));
		Value.ReplaceInline(TEXT(":"), TEXT("_"));
		return Value.IsEmpty() ? Fallback : Value;
	}

	static void CollectStringIdsFromObjects(
		const TArray<TSharedPtr<FJsonValue>>* Values,
		const TArray<FString>& IdFields,
		TSet<FString>& OutIds)
	{
		if (!Values)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Id;
			if (ReadStringFieldAny(Obj, IdFields, Id))
			{
				OutIds.Add(Id);
			}
		}
	}

	static bool StringSetContainsAny(const TSet<FString>& Values, const TArray<FString>& Candidates)
	{
		for (const FString& Candidate : Candidates)
		{
			if (!Candidate.IsEmpty() && Values.Contains(Candidate))
			{
				return true;
			}
		}
		return false;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildSettlementLayoutFollowupPlan()
	{
		TArray<TSharedPtr<FJsonValue>> Steps;

		TSharedRef<FJsonObject> AssemblyPlan = MakeArchitectureFollowupStep(
			1,
			TEXT("architecture_modular_assembly_plan"),
			TEXT("plan"),
			TEXT("Convert generated settlement buildings into modular assembly plans."));
		FollowupArgs(AssemblyPlan)->SetStringField(TEXT("settlement_plan"), TEXT("$settlement_layout_plan_build.settlement_plan"));
		FollowupArgs(AssemblyPlan)->SetStringField(TEXT("kit_manifest"), TEXT("$input.kit_manifest"));
		Steps.Add(MakeShared<FJsonValueObject>(AssemblyPlan));

		TSharedRef<FJsonObject> AssemblyExecute = MakeArchitectureFollowupStep(
			2,
			TEXT("architecture_assembly_execute"),
			TEXT("level_write"),
			TEXT("Spawn/update modular building actors from the approved assembly plan."));
		FollowupArgs(AssemblyExecute)->SetStringField(TEXT("execute"), TEXT("$input.execute"));
		FollowupArgs(AssemblyExecute)->SetStringField(TEXT("assembly_plan"), TEXT("$architecture_modular_assembly_plan"));
		Steps.Add(MakeShared<FJsonValueObject>(AssemblyExecute));

		TSharedRef<FJsonObject> Collision = MakeArchitectureFollowupStep(
			3,
			TEXT("architecture_collision_audit"),
			TEXT("read"),
			TEXT("Audit spawned building collision before navigation or production gate."));
		FollowupArgs(Collision)->SetStringField(TEXT("receipt"), TEXT("$architecture_assembly_execute"));
		Steps.Add(MakeShared<FJsonValueObject>(Collision));

		TSharedRef<FJsonObject> Entrances = MakeArchitectureFollowupStep(
			4,
			TEXT("settlement_road_to_entrance_validate"),
			TEXT("validate"),
			TEXT("Validate every generated building entrance connects to the settlement road graph."));
		FollowupArgs(Entrances)->SetStringField(TEXT("settlement_plan"), TEXT("$settlement_layout_plan_build.settlement_plan"));
		Steps.Add(MakeShared<FJsonValueObject>(Entrances));

		TSharedRef<FJsonObject> Reachability = MakeArchitectureFollowupStep(
			5,
			TEXT("architecture_reachability_audit"),
			TEXT("read"),
			TEXT("Audit interior and exterior reachability after assembly and collision readback."));
		FollowupArgs(Reachability)->SetStringField(TEXT("receipt"), TEXT("$architecture_assembly_execute"));
		Steps.Add(MakeShared<FJsonValueObject>(Reachability));

		TSharedRef<FJsonObject> DataLayer = MakeArchitectureFollowupStep(
			6,
			TEXT("world_create_data_layer_actor_membership_apply"),
			TEXT("level_write"),
			TEXT("Apply native DataLayer membership for settlement buildings and district actors."));
		FollowupArgs(DataLayer)->SetStringField(TEXT("execute"), TEXT("$input.execute"));
		FollowupArgs(DataLayer)->SetStringField(TEXT("data_layer_names"), TEXT("$settlement_layout_plan_build.settlement_plan.data_layers[].name"));
		FollowupArgs(DataLayer)->SetStringField(TEXT("receipt"), TEXT("$architecture_assembly_execute"));
		Steps.Add(MakeShared<FJsonValueObject>(DataLayer));

		TSharedRef<FJsonObject> Hlod = MakeArchitectureFollowupStep(
			7,
			TEXT("architecture_hlod_build_dispatch"),
			TEXT("editor_build"),
			TEXT("Dispatch HLOD generation after buildings are assembled and tagged."));
		FollowupArgs(Hlod)->SetStringField(TEXT("execute"), TEXT("$input.execute"));
		FollowupArgs(Hlod)->SetStringField(TEXT("mode"), TEXT("changed"));
		Steps.Add(MakeShared<FJsonValueObject>(Hlod));

		TSharedRef<FJsonObject> Save = MakeArchitectureFollowupStep(
			8,
			TEXT("world_create_save_validate_fast"),
			TEXT("save"),
			TEXT("Save, reload, screenshot, and envelope the settlement deployment."));
		FollowupArgs(Save)->SetStringField(TEXT("execute"), TEXT("$input.execute"));
		FollowupArgs(Save)->SetStringField(TEXT("prior_receipts"), TEXT("$all_prior_receipts"));
		Steps.Add(MakeShared<FJsonValueObject>(Save));

		TSharedRef<FJsonObject> Gate = MakeArchitectureFollowupStep(
			9,
			TEXT("architecture_production_gate_validate"),
			TEXT("receipt_validate"),
			TEXT("Fail closed unless settlement layout, assembly, collision, reachability, DataLayer, HLOD, save, reload, and screenshot receipts are present."));
		FollowupArgs(Gate)->SetStringField(TEXT("require_worldforge_chain"), TEXT("true"));
		FollowupArgs(Gate)->SetStringField(TEXT("prior_receipts"), TEXT("$all_prior_receipts"));
		Steps.Add(MakeShared<FJsonValueObject>(Gate));

		return Steps;
	}

	static TSharedRef<FJsonObject> SettlementFootprintJson(double WidthCm, double DepthCm, int32 Floors)
	{
		TSharedRef<FJsonObject> Footprint = MakeShared<FJsonObject>();
		Footprint->SetNumberField(TEXT("width_cm"), WidthCm);
		Footprint->SetNumberField(TEXT("depth_cm"), DepthCm);
		Footprint->SetNumberField(TEXT("floors"), Floors);
		Footprint->SetNumberField(TEXT("approx_height_cm"), FMath::Max(1, Floors) * 320.0);
		return Footprint;
	}

	static bool RunSettlementLayoutPlanBuild(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("settlement_layout_plan_build"));
		TSharedPtr<FJsonObject> Plan = ResolveNamedPlanObject(Arguments, TEXT("settlement_plan"));

		FString SettlementId;
		ReadStringFieldAny(Plan, { TEXT("id"), TEXT("settlement_id"), TEXT("name") }, SettlementId);
		if (SettlementId.IsEmpty())
		{
			ReadStringFieldAny(Arguments, { TEXT("id"), TEXT("settlement_id"), TEXT("name") }, SettlementId);
		}
		SettlementId = MakeArchitectureIdToken(SettlementId, TEXT("settlement_auto"));

		FString StyleId;
		ReadStringFieldAny(Plan, { TEXT("style_id"), TEXT("architecture_style"), TEXT("style") }, StyleId);
		if (StyleId.IsEmpty())
		{
			ReadStringFieldAny(Arguments, { TEXT("style_id"), TEXT("architecture_style"), TEXT("style") }, StyleId);
		}
		StyleId = MakeArchitectureIdToken(StyleId, TEXT("somol_modular_default"));

		FVector Center = FVector::ZeroVector;
		if (!TryReadVectorField(Plan, TEXT("center_cm"), Center) && !TryReadVectorField(Plan, TEXT("center"), Center))
		{
			TryReadVectorField(Arguments, TEXT("center_cm"), Center);
		}

		double RadiusCm = 6000.0;
		ReadNumberFieldAny(Plan, { TEXT("radius_cm"), TEXT("settlement_radius_cm") }, RadiusCm);
		ReadNumberFieldAny(Arguments, { TEXT("radius_cm"), TEXT("settlement_radius_cm") }, RadiusCm);
		const double RawRadiusCm = RadiusCm;
		RadiusCm = FMath::Clamp(RadiusCm, 1000.0, 200000.0);

		double BuildingCountValue = 48.0;
		ReadNumberFieldAny(Plan, { TEXT("building_count"), TEXT("target_building_count") }, BuildingCountValue);
		ReadNumberFieldAny(Arguments, { TEXT("building_count"), TEXT("target_building_count") }, BuildingCountValue);
		const int32 TotalBuildingCount = FMath::Clamp(FMath::RoundToInt(BuildingCountValue), 1, 10000000);

		double BuildingOffsetValue = 0.0;
		ReadNumberFieldAny(Arguments, { TEXT("building_offset"), TEXT("page_offset") }, BuildingOffsetValue);
		const int32 BuildingOffset = FMath::Clamp(FMath::RoundToInt(BuildingOffsetValue), 0, TotalBuildingCount - 1);

		double BuildingPageSizeValue = 512.0;
		ReadNumberFieldAny(Arguments, { TEXT("building_page_size"), TEXT("page_size") }, BuildingPageSizeValue);
		const int32 BuildingPageSize = FMath::Clamp(FMath::RoundToInt(BuildingPageSizeValue), 1, 512);
		const int32 BuildingEndExclusive = FMath::Min(TotalBuildingCount, BuildingOffset + BuildingPageSize);
		const int32 BuildingCount = BuildingEndExclusive - BuildingOffset;

		double DistrictCountValue = 5.0;
		ReadNumberFieldAny(Plan, { TEXT("district_count"), TEXT("target_district_count") }, DistrictCountValue);
		ReadNumberFieldAny(Arguments, { TEXT("district_count"), TEXT("target_district_count") }, DistrictCountValue);
		const int32 DistrictCount = FMath::Clamp(FMath::RoundToInt(DistrictCountValue), 1, 32);

		double RoadRingCountValue = 2.0;
		ReadNumberFieldAny(Plan, { TEXT("road_ring_count"), TEXT("ring_count") }, RoadRingCountValue);
		ReadNumberFieldAny(Arguments, { TEXT("road_ring_count"), TEXT("ring_count") }, RoadRingCountValue);
		const int32 RoadRingCount = FMath::Clamp(FMath::RoundToInt(RoadRingCountValue), 1, 8);

		TArray<FString> DistrictTypes;
		ReadStringArrayFieldAny(Plan, { TEXT("district_types"), TEXT("district_palette") }, DistrictTypes);
		if (DistrictTypes.IsEmpty())
		{
			ReadStringArrayFieldAny(Arguments, { TEXT("district_types"), TEXT("district_palette") }, DistrictTypes);
		}
		if (DistrictTypes.IsEmpty())
		{
			DistrictTypes.Add(TEXT("residential"));
			DistrictTypes.Add(TEXT("market"));
			DistrictTypes.Add(TEXT("craft"));
			DistrictTypes.Add(TEXT("military"));
			DistrictTypes.Add(TEXT("civic"));
			DistrictTypes.Add(TEXT("harbor"));
		}

		TArray<FString> Archetypes;
		ReadStringArrayFieldAny(Plan, { TEXT("building_archetypes"), TEXT("archetypes") }, Archetypes);
		if (Archetypes.IsEmpty())
		{
			ReadStringArrayFieldAny(Arguments, { TEXT("building_archetypes"), TEXT("archetypes") }, Archetypes);
		}
		if (Archetypes.IsEmpty())
		{
			Archetypes.Add(TEXT("house"));
			Archetypes.Add(TEXT("shop"));
			Archetypes.Add(TEXT("workshop"));
			Archetypes.Add(TEXT("barracks"));
			Archetypes.Add(TEXT("hall"));
			Archetypes.Add(TEXT("warehouse"));
		}

		TArray<FString> Warnings;
		if (!FMath::IsNearlyEqual(RawRadiusCm, RadiusCm))
		{
			Warnings.Add(TEXT("radius_clamped_to_supported_range"));
		}
		if (FMath::RoundToInt(BuildingCountValue) != TotalBuildingCount)
		{
			Warnings.Add(TEXT("total_building_count_clamped_to_supported_range"));
		}
		if (BuildingOffset > 0 || BuildingEndExclusive < TotalBuildingCount)
		{
			Warnings.Add(TEXT("layout_page_partial_use_next_building_offset"));
		}
		if (FMath::RoundToInt(DistrictCountValue) != DistrictCount)
		{
			Warnings.Add(TEXT("district_count_clamped_to_supported_range"));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("id"), SettlementId);
		Result->SetStringField(TEXT("style_id"), StyleId);
		Result->SetObjectField(TEXT("center_cm"), VectorJson(Center));
		Result->SetNumberField(TEXT("radius_cm"), RadiusCm);
		Result->SetNumberField(TEXT("district_count"), DistrictCount);
		Result->SetNumberField(TEXT("building_count"), BuildingCount);
		Result->SetNumberField(TEXT("total_building_count"), TotalBuildingCount);
		Result->SetNumberField(TEXT("building_offset"), BuildingOffset);
		Result->SetNumberField(TEXT("building_page_size"), BuildingPageSize);
		Result->SetNumberField(TEXT("next_building_offset"), BuildingEndExclusive);
		Result->SetBoolField(TEXT("has_more_buildings"), BuildingEndExclusive < TotalBuildingCount);
		Result->SetNumberField(TEXT("road_ring_count"), RoadRingCount);

		TArray<TSharedPtr<FJsonValue>> DataLayers;
		TSharedRef<FJsonObject> RootLayer = MakeShared<FJsonObject>();
		RootLayer->SetStringField(TEXT("name"), FString::Printf(TEXT("WF_%s"), *SettlementId));
		RootLayer->SetStringField(TEXT("purpose"), TEXT("settlement_root"));
		DataLayers.Add(MakeShared<FJsonValueObject>(RootLayer));

		TArray<TSharedPtr<FJsonValue>> HlodLayers;
		TSharedRef<FJsonObject> CloseHlod = MakeShared<FJsonObject>();
		CloseHlod->SetStringField(TEXT("name"), FString::Printf(TEXT("%s_architecture_close"), *SettlementId));
		CloseHlod->SetStringField(TEXT("purpose"), TEXT("near_building_cluster"));
		HlodLayers.Add(MakeShared<FJsonValueObject>(CloseHlod));
		TSharedRef<FJsonObject> FarHlod = MakeShared<FJsonObject>();
		FarHlod->SetStringField(TEXT("name"), FString::Printf(TEXT("%s_architecture_far"), *SettlementId));
		FarHlod->SetStringField(TEXT("purpose"), TEXT("distant_city_silhouette"));
		HlodLayers.Add(MakeShared<FJsonValueObject>(FarHlod));

		TArray<TSharedPtr<FJsonValue>> Districts;
		TArray<TSharedPtr<FJsonValue>> RoadNodes;
		TArray<TSharedPtr<FJsonValue>> Roads;
		TArray<TSharedPtr<FJsonValue>> Parcels;
		TArray<TSharedPtr<FJsonValue>> Buildings;
		TArray<TSharedPtr<FJsonValue>> Entrances;
		TArray<FString> HubIds;
		HubIds.Reserve(DistrictCount);

		TSharedRef<FJsonObject> CenterRoad = MakeShared<FJsonObject>();
		CenterRoad->SetStringField(TEXT("id"), TEXT("road_center"));
		CenterRoad->SetStringField(TEXT("kind"), TEXT("plaza"));
		CenterRoad->SetObjectField(TEXT("location_cm"), VectorJson(Center));
		RoadNodes.Add(MakeShared<FJsonValueObject>(CenterRoad));

		for (int32 DistrictIndex = 0; DistrictIndex < DistrictCount; ++DistrictIndex)
		{
			const double Angle = (2.0 * PI * static_cast<double>(DistrictIndex)) / static_cast<double>(DistrictCount);
			const FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.0);
			const FVector HubLocation = Center + Direction * (RadiusCm * 0.45);
			const FString Type = MakeArchitectureIdToken(DistrictTypes[DistrictIndex % DistrictTypes.Num()], TEXT("district"));
			const FString DistrictId = FString::Printf(TEXT("%s_district_%02d_%s"), *SettlementId, DistrictIndex + 1, *Type);
			const FString HubId = FString::Printf(TEXT("%s_hub"), *DistrictId);
			HubIds.Add(HubId);

			TSharedRef<FJsonObject> District = MakeShared<FJsonObject>();
			District->SetStringField(TEXT("id"), DistrictId);
			District->SetStringField(TEXT("type"), Type);
			District->SetStringField(TEXT("road_node_id"), HubId);
			District->SetStringField(TEXT("data_layer_name"), FString::Printf(TEXT("WF_%s_%s"), *SettlementId, *Type));
			District->SetStringField(TEXT("hlod_layer_name"), DistrictIndex % 2 == 0 ? CloseHlod->GetStringField(TEXT("name")) : FarHlod->GetStringField(TEXT("name")));
			District->SetObjectField(TEXT("center_cm"), VectorJson(HubLocation));
			District->SetNumberField(TEXT("radius_cm"), RadiusCm / FMath::Max(2, DistrictCount));
			Districts.Add(MakeShared<FJsonValueObject>(District));

			TSharedRef<FJsonObject> DistrictLayer = MakeShared<FJsonObject>();
			DistrictLayer->SetStringField(TEXT("name"), FString::Printf(TEXT("WF_%s_%s"), *SettlementId, *Type));
			DistrictLayer->SetStringField(TEXT("purpose"), FString::Printf(TEXT("%s_district"), *Type));
			DataLayers.Add(MakeShared<FJsonValueObject>(DistrictLayer));

			TSharedRef<FJsonObject> RoadNode = MakeShared<FJsonObject>();
			RoadNode->SetStringField(TEXT("id"), HubId);
			RoadNode->SetStringField(TEXT("kind"), TEXT("district_hub"));
			RoadNode->SetStringField(TEXT("district_id"), DistrictId);
			RoadNode->SetObjectField(TEXT("location_cm"), VectorJson(HubLocation));
			RoadNodes.Add(MakeShared<FJsonValueObject>(RoadNode));

			TSharedRef<FJsonObject> Radial = MakeShared<FJsonObject>();
			Radial->SetStringField(TEXT("id"), FString::Printf(TEXT("road_radial_%02d"), DistrictIndex + 1));
			Radial->SetStringField(TEXT("kind"), TEXT("radial"));
			Radial->SetStringField(TEXT("from"), TEXT("road_center"));
			Radial->SetStringField(TEXT("to"), HubId);
			Roads.Add(MakeShared<FJsonValueObject>(Radial));
		}

		for (int32 DistrictIndex = 0; DistrictIndex < DistrictCount; ++DistrictIndex)
		{
			TSharedRef<FJsonObject> Ring = MakeShared<FJsonObject>();
			Ring->SetStringField(TEXT("id"), FString::Printf(TEXT("road_ring_%02d"), DistrictIndex + 1));
			Ring->SetStringField(TEXT("kind"), TEXT("ring"));
			Ring->SetStringField(TEXT("from"), HubIds[DistrictIndex]);
			Ring->SetStringField(TEXT("to"), HubIds[(DistrictIndex + 1) % DistrictCount]);
			Roads.Add(MakeShared<FJsonValueObject>(Ring));
		}

		TArray<int32> DistrictBuildingCounts;
		DistrictBuildingCounts.SetNum(DistrictCount);
		for (int32 DistrictIndex = 0; DistrictIndex < DistrictCount; ++DistrictIndex)
		{
			DistrictBuildingCounts[DistrictIndex] = TotalBuildingCount / DistrictCount
				+ (DistrictIndex < (TotalBuildingCount % DistrictCount) ? 1 : 0);
		}

		for (int32 BuildingIndex = BuildingOffset; BuildingIndex < BuildingEndExclusive; ++BuildingIndex)
		{
			const int32 DistrictIndex = BuildingIndex % DistrictCount;
			const int32 LocalIndex = BuildingIndex / DistrictCount;
			const int32 LocalCount = FMath::Max(1, DistrictBuildingCounts[DistrictIndex]);
			const double BaseAngle = (2.0 * PI * static_cast<double>(DistrictIndex)) / static_cast<double>(DistrictCount);
			const double Offset = (static_cast<double>((LocalIndex % 5) - 2) * 0.075) + (static_cast<double>(LocalIndex / 5) * 0.035);
			const double RingFraction = static_cast<double>(LocalIndex + 1) / static_cast<double>(LocalCount + 1);
			const double Distance = RadiusCm * (0.18 + 0.62 * RingFraction);
			const double Angle = BaseAngle + Offset;
			const FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.0);
			const FVector Location = Center + Direction * Distance;
			const FVector HubDirection = FVector(FMath::Cos(BaseAngle), FMath::Sin(BaseAngle), 0.0);
			const FString DistrictType = MakeArchitectureIdToken(DistrictTypes[DistrictIndex % DistrictTypes.Num()], TEXT("district"));
			const FString Archetype = MakeArchitectureIdToken(Archetypes[BuildingIndex % Archetypes.Num()], TEXT("house"));
			const FString BuildingId = FString::Printf(TEXT("%s_building_%03d"), *SettlementId, BuildingIndex + 1);
			const FString DistrictId = FString::Printf(TEXT("%s_district_%02d_%s"), *SettlementId, DistrictIndex + 1, *DistrictType);
			const FString EntranceId = FString::Printf(TEXT("%s_entrance"), *BuildingId);
			const double WidthCm = DistrictType == TEXT("military") ? 1400.0 : (Archetype == TEXT("hall") ? 1600.0 : 900.0);
			const double DepthCm = DistrictType == TEXT("market") ? 1200.0 : (Archetype == TEXT("warehouse") ? 1800.0 : 850.0);
			const int32 Floors = DistrictType == TEXT("civic") ? 3 : (DistrictType == TEXT("military") ? 2 : 1 + (BuildingIndex % 3 == 0 ? 1 : 0));
			const double YawDeg = FMath::RadiansToDegrees(FMath::Atan2(Center.Y - Location.Y, Center.X - Location.X));
			const FVector EntranceLocation = Location - HubDirection * (DepthCm * 0.5 + 120.0);

			TSharedRef<FJsonObject> Parcel = MakeShared<FJsonObject>();
			Parcel->SetStringField(TEXT("id"), FString::Printf(TEXT("%s_parcel"), *BuildingId));
			Parcel->SetStringField(TEXT("district_id"), DistrictId);
			Parcel->SetStringField(TEXT("building_id"), BuildingId);
			Parcel->SetObjectField(TEXT("center_cm"), VectorJson(Location));
			Parcel->SetObjectField(TEXT("footprint_cm"), SettlementFootprintJson(WidthCm + 300.0, DepthCm + 300.0, Floors));
			Parcels.Add(MakeShared<FJsonValueObject>(Parcel));

			TSharedRef<FJsonObject> Building = MakeShared<FJsonObject>();
			Building->SetStringField(TEXT("id"), BuildingId);
			Building->SetStringField(TEXT("district_id"), DistrictId);
			Building->SetStringField(TEXT("archetype"), Archetype);
			Building->SetStringField(TEXT("style_id"), StyleId);
			Building->SetStringField(TEXT("data_layer_name"), FString::Printf(TEXT("WF_%s_%s"), *SettlementId, *DistrictType));
			Building->SetStringField(TEXT("hlod_layer_name"), BuildingIndex % 3 == 0 ? FarHlod->GetStringField(TEXT("name")) : CloseHlod->GetStringField(TEXT("name")));
			Building->SetStringField(TEXT("entrance_id"), EntranceId);
			Building->SetObjectField(TEXT("location_cm"), VectorJson(Location));
			Building->SetNumberField(TEXT("rotation_yaw_degrees"), YawDeg);
			Building->SetObjectField(TEXT("footprint_cm"), SettlementFootprintJson(WidthCm, DepthCm, Floors));
			TSharedRef<FJsonObject> AssemblyHint = MakeShared<FJsonObject>();
			AssemblyHint->SetStringField(TEXT("kit_style_id"), StyleId);
			AssemblyHint->SetStringField(TEXT("archetype"), Archetype);
			AssemblyHint->SetStringField(TEXT("orientation_policy"), TEXT("front_faces_connected_road"));
			AssemblyHint->SetStringField(TEXT("scale_policy"), TEXT("centimeter_exact_socket_grid"));
			Building->SetObjectField(TEXT("assembly_hint"), AssemblyHint);
			Buildings.Add(MakeShared<FJsonValueObject>(Building));

			TSharedRef<FJsonObject> Entrance = MakeShared<FJsonObject>();
			Entrance->SetStringField(TEXT("id"), EntranceId);
			Entrance->SetStringField(TEXT("building_id"), BuildingId);
			Entrance->SetStringField(TEXT("district_id"), DistrictId);
			Entrance->SetStringField(TEXT("connected_road_id"), HubIds[DistrictIndex]);
			Entrance->SetObjectField(TEXT("location_cm"), VectorJson(EntranceLocation));
			Entrances.Add(MakeShared<FJsonValueObject>(Entrance));
		}

		Result->SetArrayField(TEXT("districts"), Districts);
		Result->SetArrayField(TEXT("road_nodes"), RoadNodes);
		Result->SetArrayField(TEXT("roads"), Roads);
		Result->SetArrayField(TEXT("parcels"), Parcels);
		Result->SetArrayField(TEXT("buildings"), Buildings);
		Result->SetArrayField(TEXT("entrances"), Entrances);
		Result->SetArrayField(TEXT("data_layers"), DataLayers);
		Result->SetArrayField(TEXT("hlod_layers"), HlodLayers);

		const bool bOk = BuildingCount > 0 && DistrictCount > 0 && RoadNodes.Num() > 1 && Entrances.Num() == BuildingCount;
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("settlement_layout_plan_ready") : TEXT("settlement_layout_plan_failed"));
		Out->SetObjectField(TEXT("settlement_plan"), Result);
		Out->SetNumberField(TEXT("district_count"), DistrictCount);
		Out->SetNumberField(TEXT("building_count"), BuildingCount);
		Out->SetNumberField(TEXT("total_building_count"), TotalBuildingCount);
		Out->SetNumberField(TEXT("building_offset"), BuildingOffset);
		Out->SetNumberField(TEXT("next_building_offset"), BuildingEndExclusive);
		Out->SetBoolField(TEXT("has_more_buildings"), BuildingEndExclusive < TotalBuildingCount);
		Out->SetNumberField(TEXT("road_node_count"), RoadNodes.Num());
		Out->SetNumberField(TEXT("road_edge_count"), Roads.Num());
		Out->SetNumberField(TEXT("entrance_count"), Entrances.Num());
		Out->SetArrayField(TEXT("warnings"), StringArrayJson(Warnings));
		Out->SetArrayField(TEXT("worldforge_followup_plan"), BuildSettlementLayoutFollowupPlan());
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%s:%d:%d:%d:%d:%d:%f:%f"), *SettlementId, *StyleId, DistrictCount, TotalBuildingCount, BuildingOffset, BuildingCount, RoadNodes.Num(), Center.X, Center.Y)));
		Summary = FString::Printf(TEXT("settlement_layout_plan_build planned page %d..%d of %d buildings across %d districts."), BuildingOffset, BuildingEndExclusive, TotalBuildingCount, DistrictCount);
		if (!bOk)
		{
			Error = TEXT("Settlement layout planning failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool TryResolveKitModules(
		const TSharedRef<FJsonObject>& Arguments,
		const TArray<TSharedPtr<FJsonValue>>*& OutModules)
	{
		OutModules = nullptr;
		if (const TSharedPtr<FJsonObject>* KitPtr = nullptr; Arguments->TryGetObjectField(TEXT("kit_manifest"), KitPtr) && KitPtr && KitPtr->IsValid())
		{
			if ((*KitPtr)->TryGetArrayField(TEXT("module_descriptors"), OutModules) && OutModules)
			{
				return true;
			}
			if ((*KitPtr)->TryGetArrayField(TEXT("modules"), OutModules) && OutModules)
			{
				return true;
			}
		}
		if (Arguments->TryGetArrayField(TEXT("module_descriptors"), OutModules) && OutModules)
		{
			return true;
		}
		if (Arguments->TryGetArrayField(TEXT("kit_modules"), OutModules) && OutModules)
		{
			return true;
		}
		return false;
	}

	static bool ResolveModuleDescriptors(const TSharedRef<FJsonObject>& Arguments, const TArray<TSharedPtr<FJsonValue>>*& OutModules)
	{
		if (Arguments->TryGetArrayField(TEXT("module_descriptors"), OutModules) && OutModules)
		{
			return true;
		}
		if (Arguments->TryGetArrayField(TEXT("kit_modules"), OutModules) && OutModules)
		{
			return true;
		}
		for (const TCHAR* FieldName : { TEXT("modular_kit_manifest"), TEXT("kit_manifest") })
		{
			const TSharedPtr<FJsonObject>* Manifest = nullptr;
			if (Arguments->TryGetObjectField(FieldName, Manifest) && Manifest && Manifest->IsValid())
			{
				if ((*Manifest)->TryGetArrayField(TEXT("module_descriptors"), OutModules) && OutModules)
				{
					return true;
				}
				if ((*Manifest)->TryGetArrayField(TEXT("modules"), OutModules) && OutModules)
				{
					return true;
				}
			}
		}
		return false;
	}

	static FString ReadModuleAssetPath(const TSharedPtr<FJsonObject>& Module)
	{
		FString AssetPath;
		ReadStringFieldAny(Module, { TEXT("asset_path"), TEXT("static_mesh"), TEXT("mesh_path"), TEXT("asset") }, AssetPath);
		return AssetPath;
	}

	static double ReadNestedNumberOrDefault(const TSharedPtr<FJsonObject>& Object, const TCHAR* ChildField, const TArray<FString>& Fields, double DefaultValue)
	{
		if (!Object.IsValid())
		{
			return DefaultValue;
		}
		double Value = DefaultValue;
		if (ReadNumberFieldAny(Object, Fields, Value))
		{
			return Value;
		}
		if (const TSharedPtr<FJsonObject>* Child = nullptr; Object->TryGetObjectField(ChildField, Child) && Child && Child->IsValid())
		{
			Value = DefaultValue;
			if (ReadNumberFieldAny(*Child, Fields, Value))
			{
				return Value;
			}
		}
		return DefaultValue;
	}

	static TSharedRef<FJsonObject> TransformContractJson(double GridUnitCm, const FString& SocketPolicy)
	{
		TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
		Contract->SetNumberField(TEXT("grid_unit_cm"), GridUnitCm);
		Contract->SetStringField(TEXT("socket_policy"), SocketPolicy);
		Contract->SetStringField(TEXT("scale_policy"), TEXT("centimeter_exact_socket_grid"));
		Contract->SetStringField(TEXT("orientation_policy"), TEXT("front_faces_connected_road_or_building_center"));
		return Contract;
	}

	static TSharedRef<FJsonObject> RotatorJson(const FRotator& Value)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	static FVector ScaleJsonSafe(double X, double Y, double Z)
	{
		return FVector(FMath::Max(0.01, X), FMath::Max(0.01, Y), FMath::Max(0.01, Z));
	}

	static FVector RotateSettlementOffset(const FVector& Offset, double YawDegrees)
	{
		const double Radians = FMath::DegreesToRadians(YawDegrees);
		const double Cos = FMath::Cos(Radians);
		const double Sin = FMath::Sin(Radians);
		return FVector(
			Offset.X * Cos - Offset.Y * Sin,
			Offset.X * Sin + Offset.Y * Cos,
			Offset.Z);
	}

	static void AddAssemblyModule(
		TArray<TSharedPtr<FJsonValue>>& Modules,
		const FString& AssemblyId,
		const FString& BuildingId,
		const FString& ModuleSuffix,
		const FString& Kind,
		const FString& AssetPath,
		const FVector& Location,
		const FRotator& Rotation,
		const FVector& Scale,
		double GridUnitCm,
		const FString& SocketPolicy)
	{
		TSharedRef<FJsonObject> Module = MakeShared<FJsonObject>();
		const FString ModuleId = FString::Printf(TEXT("%s_%s_%s"), *BuildingId, *Kind, *ModuleSuffix);
		Module->SetStringField(TEXT("module_id"), ModuleId);
		Module->SetStringField(TEXT("building_id"), BuildingId);
		Module->SetStringField(TEXT("kind"), Kind);
		Module->SetStringField(TEXT("asset_path"), AssetPath);
		Module->SetStringField(TEXT("actor_label"), FString::Printf(TEXT("SOMOL_%s_%s"), *AssemblyId, *ModuleId));
		Module->SetObjectField(TEXT("location_cm"), VectorJson(Location));
		Module->SetObjectField(TEXT("rotation_deg"), RotatorJson(Rotation));
		Module->SetObjectField(TEXT("scale_xyz"), VectorJson(Scale));
		Module->SetStringField(TEXT("collision_profile"), TEXT("BlockAll"));
		Module->SetBoolField(TEXT("nav_relevant"), true);
		Module->SetObjectField(TEXT("transform_contract"), TransformContractJson(GridUnitCm, SocketPolicy));
		Modules.Add(MakeShared<FJsonValueObject>(Module));
	}

	static bool RunArchitectureModularAssemblyPlan(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error,
		const FString& ToolName)
	{
		SetArchitectureReceiptBase(Out, ToolName);

		TSharedPtr<FJsonObject> SettlementPlan = ResolveNamedPlanObject(Arguments, TEXT("settlement_plan"));
		const TArray<TSharedPtr<FJsonValue>>* Buildings = nullptr;
		if (!SettlementPlan.IsValid() || !SettlementPlan->TryGetArrayField(TEXT("buildings"), Buildings) || !Buildings)
		{
			if (!Arguments->TryGetArrayField(TEXT("buildings"), Buildings) || !Buildings)
			{
				Out->SetBoolField(TEXT("ok"), false);
				SololmcpError::MissingParam(Out, TEXT("settlement_plan.buildings"));
				Error = TEXT("Missing settlement_plan.buildings or buildings.");
				return false;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* KitModules = nullptr;
		if (!TryResolveKitModules(Arguments, KitModules) || !KitModules)
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("kit_manifest.module_descriptors"));
			Error = TEXT("Missing kit_manifest.module_descriptors, kit_manifest.modules, or module_descriptors.");
			return false;
		}

		TMap<FString, TSharedPtr<FJsonObject>> ModuleByKind;
		for (const TSharedPtr<FJsonValue>& Value : *KitModules)
		{
			const TSharedPtr<FJsonObject> Module = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Kind;
			if (!ReadStringFieldAny(Module, { TEXT("kind"), TEXT("module_kind"), TEXT("role") }, Kind))
			{
				continue;
			}
			Kind = MakeArchitectureIdToken(Kind.ToLower(), TEXT("module"));
			if (!ReadModuleAssetPath(Module).IsEmpty() && !ModuleByKind.Contains(Kind))
			{
				ModuleByKind.Add(Kind, Module);
			}
		}

		TArray<FString> RequiredKinds = { TEXT("foundation"), TEXT("floor"), TEXT("wall"), TEXT("door"), TEXT("roof") };
		TArray<FString> MissingKinds;
		for (const FString& Kind : RequiredKinds)
		{
			if (!ModuleByKind.Contains(Kind))
			{
				MissingKinds.Add(Kind);
			}
		}
		if (!MissingKinds.IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("status"), TEXT("modular_assembly_plan_failed_missing_kit_modules"));
			Out->SetArrayField(TEXT("missing_kinds"), StringArrayJson(MissingKinds));
			Error = TEXT("Modular kit is missing required module kinds.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		FString SettlementId;
		ReadStringFieldAny(SettlementPlan, { TEXT("id"), TEXT("settlement_id"), TEXT("name") }, SettlementId);
		SettlementId = MakeArchitectureIdToken(SettlementId, TEXT("settlement"));

		FString AssemblyId;
		ReadStringFieldAny(Arguments, { TEXT("assembly_id") }, AssemblyId);
		if (AssemblyId.IsEmpty())
		{
			AssemblyId = FString::Printf(TEXT("%s_modular_assembly"), *SettlementId);
		}
		AssemblyId = MakeArchitectureIdToken(AssemblyId, TEXT("assembly"));

		FString StyleId;
		ReadStringFieldAny(SettlementPlan, { TEXT("style_id"), TEXT("style") }, StyleId);
		ReadStringFieldAny(Arguments, { TEXT("style_id"), TEXT("style") }, StyleId);
		StyleId = MakeArchitectureIdToken(StyleId, TEXT("somol_modular_default"));

		double GridUnitCm = 100.0;
		ReadNumberFieldAny(Arguments, { TEXT("grid_unit_cm") }, GridUnitCm);
		double FloorHeightCm = 320.0;
		ReadNumberFieldAny(Arguments, { TEXT("floor_height_cm") }, FloorHeightCm);
		GridUnitCm = FMath::Clamp(GridUnitCm, 10.0, 1000.0);
		FloorHeightCm = FMath::Clamp(FloorHeightCm, 180.0, 800.0);

		bool bIncludeInteriorStairs = true;
		Arguments->TryGetBoolField(TEXT("include_interior_stairs"), bIncludeInteriorStairs);

		int32 MaxBuildings = 64;
		Arguments->TryGetNumberField(TEXT("max_buildings_per_plan"), MaxBuildings);
		MaxBuildings = FMath::Clamp(MaxBuildings, 1, 512);
		if (Buildings->Num() > MaxBuildings)
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("status"), TEXT("modular_assembly_plan_failed_building_limit"));
			Out->SetNumberField(TEXT("building_count"), Buildings->Num());
			Out->SetNumberField(TEXT("max_buildings_per_plan"), MaxBuildings);
			Error = TEXT("Building count exceeds max_buildings_per_plan.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		auto AssetForKind = [&ModuleByKind](const FString& Kind) -> FString
		{
			if (const TSharedPtr<FJsonObject>* Module = ModuleByKind.Find(Kind))
			{
				return ReadModuleAssetPath(*Module);
			}
			return FString();
		};

		const FString FoundationAsset = AssetForKind(TEXT("foundation"));
		const FString FloorAsset = AssetForKind(TEXT("floor"));
		const FString WallAsset = AssetForKind(TEXT("wall"));
		const FString DoorAsset = AssetForKind(TEXT("door"));
		const FString RoofAsset = AssetForKind(TEXT("roof"));
		const FString StairAsset = AssetForKind(TEXT("stair"));

		TArray<TSharedPtr<FJsonValue>> OutputModules;
		TArray<TSharedPtr<FJsonValue>> BuildingRows;
		TArray<FString> Warnings;

		for (int32 BuildingIndex = 0; BuildingIndex < Buildings->Num(); ++BuildingIndex)
		{
			const TSharedPtr<FJsonObject> Building = (*Buildings)[BuildingIndex].IsValid() ? (*Buildings)[BuildingIndex]->AsObject() : nullptr;
			if (!Building.IsValid())
			{
				Warnings.Add(FString::Printf(TEXT("building[%d]_not_object_skipped"), BuildingIndex));
				continue;
			}

			FString BuildingId;
			ReadStringFieldAny(Building, { TEXT("id"), TEXT("building_id"), TEXT("name") }, BuildingId);
			BuildingId = MakeArchitectureIdToken(BuildingId, FString::Printf(TEXT("building_%03d"), BuildingIndex + 1));
			FString Archetype;
			ReadStringFieldAny(Building, { TEXT("archetype"), TEXT("type") }, Archetype);
			Archetype = MakeArchitectureIdToken(Archetype, TEXT("building"));

			FVector Location = FVector::ZeroVector;
			TryReadVectorField(Building, TEXT("location_cm"), Location);
			double YawDegrees = 0.0;
			ReadNumberFieldAny(Building, { TEXT("rotation_yaw_degrees"), TEXT("yaw_degrees"), TEXT("yaw") }, YawDegrees);
			const double WidthCm = ReadNestedNumberOrDefault(Building, TEXT("footprint_cm"), { TEXT("width_cm"), TEXT("width") }, 900.0);
			const double DepthCm = ReadNestedNumberOrDefault(Building, TEXT("footprint_cm"), { TEXT("depth_cm"), TEXT("depth") }, 900.0);
			const int32 Floors = FMath::Clamp(FMath::RoundToInt(ReadNestedNumberOrDefault(Building, TEXT("footprint_cm"), { TEXT("floors"), TEXT("floor_count") }, 1.0)), 1, 12);
			const int32 FirstModuleIndex = OutputModules.Num();

			AddAssemblyModule(
				OutputModules,
				AssemblyId,
				BuildingId,
				TEXT("base"),
				TEXT("foundation"),
				FoundationAsset,
				Location,
				FRotator(0.0, YawDegrees, 0.0),
				ScaleJsonSafe(WidthCm / GridUnitCm, DepthCm / GridUnitCm, 1.0),
				GridUnitCm,
				TEXT("foundation_to_floor"));

			for (int32 FloorIndex = 0; FloorIndex < Floors; ++FloorIndex)
			{
				const double Z = Location.Z + FloorIndex * FloorHeightCm;
				const FVector FloorLocation(Location.X, Location.Y, Z);
				AddAssemblyModule(
					OutputModules,
					AssemblyId,
					BuildingId,
					FString::Printf(TEXT("floor_%02d"), FloorIndex + 1),
					TEXT("floor"),
					FloorAsset,
					FloorLocation,
					FRotator(0.0, YawDegrees, 0.0),
					ScaleJsonSafe(WidthCm / GridUnitCm, DepthCm / GridUnitCm, 1.0),
					GridUnitCm,
					TEXT("floor_to_wall"));

				const double WallZ = Z + FloorHeightCm * 0.5;
				const FVector North = Location + RotateSettlementOffset(FVector(0.0, DepthCm * 0.5, WallZ - Location.Z), YawDegrees);
				const FVector South = Location + RotateSettlementOffset(FVector(0.0, -DepthCm * 0.5, WallZ - Location.Z), YawDegrees);
				const FVector East = Location + RotateSettlementOffset(FVector(WidthCm * 0.5, 0.0, WallZ - Location.Z), YawDegrees);
				const FVector West = Location + RotateSettlementOffset(FVector(-WidthCm * 0.5, 0.0, WallZ - Location.Z), YawDegrees);

				AddAssemblyModule(OutputModules, AssemblyId, BuildingId, FString::Printf(TEXT("north_wall_%02d"), FloorIndex + 1), TEXT("wall"), WallAsset, North, FRotator(0.0, YawDegrees, 0.0), ScaleJsonSafe(WidthCm / GridUnitCm, 1.0, FloorHeightCm / GridUnitCm), GridUnitCm, TEXT("wall_edge_snap"));
				AddAssemblyModule(OutputModules, AssemblyId, BuildingId, FString::Printf(TEXT("south_wall_%02d"), FloorIndex + 1), TEXT("wall"), WallAsset, South, FRotator(0.0, YawDegrees + 180.0, 0.0), ScaleJsonSafe(WidthCm / GridUnitCm, 1.0, FloorHeightCm / GridUnitCm), GridUnitCm, TEXT("wall_edge_snap"));
				AddAssemblyModule(OutputModules, AssemblyId, BuildingId, FString::Printf(TEXT("east_wall_%02d"), FloorIndex + 1), TEXT("wall"), WallAsset, East, FRotator(0.0, YawDegrees + 90.0, 0.0), ScaleJsonSafe(DepthCm / GridUnitCm, 1.0, FloorHeightCm / GridUnitCm), GridUnitCm, TEXT("wall_edge_snap"));
				AddAssemblyModule(OutputModules, AssemblyId, BuildingId, FString::Printf(TEXT("west_wall_%02d"), FloorIndex + 1), TEXT("wall"), WallAsset, West, FRotator(0.0, YawDegrees - 90.0, 0.0), ScaleJsonSafe(DepthCm / GridUnitCm, 1.0, FloorHeightCm / GridUnitCm), GridUnitCm, TEXT("wall_edge_snap"));

				if (FloorIndex == 0)
				{
					const FVector DoorLocation = Location + RotateSettlementOffset(FVector(0.0, -DepthCm * 0.5 - 2.0, FloorHeightCm * 0.45), YawDegrees);
					AddAssemblyModule(
						OutputModules,
						AssemblyId,
						BuildingId,
						TEXT("front_door"),
						TEXT("door"),
						DoorAsset,
						DoorLocation,
						FRotator(0.0, YawDegrees + 180.0, 0.0),
						ScaleJsonSafe(1.0, 1.0, FloorHeightCm / GridUnitCm),
						GridUnitCm,
						TEXT("door_replaces_front_wall_segment"));
				}
			}

			if (bIncludeInteriorStairs && Floors > 1 && !StairAsset.IsEmpty())
			{
				const FVector StairLocation = Location + RotateSettlementOffset(FVector(WidthCm * 0.25, DepthCm * 0.20, FloorHeightCm * 0.5), YawDegrees);
				AddAssemblyModule(
					OutputModules,
					AssemblyId,
					BuildingId,
					TEXT("stair_core"),
					TEXT("stair"),
					StairAsset,
					StairLocation,
					FRotator(0.0, YawDegrees, 0.0),
					ScaleJsonSafe(1.0, 1.0, Floors),
					GridUnitCm,
					TEXT("floor_to_floor_nav"));
			}
			else if (bIncludeInteriorStairs && Floors > 1)
			{
				Warnings.Add(FString::Printf(TEXT("%s_multi_floor_without_stair_module"), *BuildingId));
			}

			const FVector RoofLocation(Location.X, Location.Y, Location.Z + Floors * FloorHeightCm);
			AddAssemblyModule(
				OutputModules,
				AssemblyId,
				BuildingId,
				TEXT("roof"),
				TEXT("roof"),
				RoofAsset,
				RoofLocation,
				FRotator(0.0, YawDegrees, 0.0),
				ScaleJsonSafe(WidthCm / GridUnitCm, DepthCm / GridUnitCm, 1.0),
				GridUnitCm,
				TEXT("roof_to_top_wall"));

			TSharedRef<FJsonObject> BuildingRow = MakeShared<FJsonObject>();
			BuildingRow->SetStringField(TEXT("building_id"), BuildingId);
			BuildingRow->SetStringField(TEXT("archetype"), Archetype);
			BuildingRow->SetNumberField(TEXT("floor_count"), Floors);
			BuildingRow->SetNumberField(TEXT("first_module_index"), FirstModuleIndex);
			BuildingRow->SetNumberField(TEXT("module_count"), OutputModules.Num() - FirstModuleIndex);
			BuildingRows.Add(MakeShared<FJsonValueObject>(BuildingRow));
		}

		TSharedRef<FJsonObject> AssemblyPlan = MakeShared<FJsonObject>();
		AssemblyPlan->SetStringField(TEXT("assembly_id"), AssemblyId);
		AssemblyPlan->SetStringField(TEXT("source_settlement_id"), SettlementId);
		AssemblyPlan->SetStringField(TEXT("style_id"), StyleId);
		AssemblyPlan->SetNumberField(TEXT("grid_unit_cm"), GridUnitCm);
		AssemblyPlan->SetNumberField(TEXT("floor_height_cm"), FloorHeightCm);
		AssemblyPlan->SetArrayField(TEXT("buildings"), BuildingRows);
		AssemblyPlan->SetArrayField(TEXT("modules"), OutputModules);
		AssemblyPlan->SetObjectField(TEXT("transform_contract"), TransformContractJson(GridUnitCm, TEXT("kit_socket_grid")));

		const bool bOk = Buildings->Num() > 0 && OutputModules.Num() > 0;
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("modular_assembly_plan_ready") : TEXT("modular_assembly_plan_failed"));
		Out->SetObjectField(TEXT("assembly_plan"), AssemblyPlan);
		Out->SetStringField(TEXT("assembly_id"), AssemblyId);
		Out->SetStringField(TEXT("source_settlement_id"), SettlementId);
		Out->SetNumberField(TEXT("building_count"), Buildings->Num());
		Out->SetNumberField(TEXT("module_count"), OutputModules.Num());
		Out->SetArrayField(TEXT("required_module_kinds"), StringArrayJson(RequiredKinds));
		Out->SetArrayField(TEXT("warnings"), StringArrayJson(Warnings));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%s:%d:%d:%f:%f"), *AssemblyId, *SettlementId, Buildings->Num(), OutputModules.Num(), GridUnitCm, FloorHeightCm)));
		Summary = FString::Printf(TEXT("%s planned %d modules for %d buildings."), *ToolName, OutputModules.Num(), Buildings->Num());
		if (!bOk)
		{
			Error = TEXT("Modular assembly plan generation failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static int32 EstimateArchitectureModulesForBuilding(const TSharedPtr<FJsonObject>& Building)
	{
		const int32 Floors = FMath::Clamp(
			FMath::RoundToInt(ReadNestedNumberOrDefault(Building, TEXT("footprint_cm"), { TEXT("floors"), TEXT("floor_count") }, 1.0)),
			1,
			32);
		return 1                 // foundation
			+ (Floors * 5)        // floor slab + four walls
			+ 1                   // front door
			+ (Floors > 1 ? 1 : 0) // stair core
			+ 1;                  // roof
	}

	static bool RunArchitectureCityBatchPlan(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_city_batch_plan"));

		TSharedPtr<FJsonObject> SettlementPlan = ResolveNamedPlanObject(Arguments, TEXT("settlement_plan"));
		const TArray<TSharedPtr<FJsonValue>>* Buildings = nullptr;
		TArray<TSharedPtr<FJsonValue>> AggregatedBuildings;
		const TArray<TSharedPtr<FJsonValue>>* SettlementPlanPages = nullptr;
		if (Arguments->TryGetArrayField(TEXT("settlement_plan_pages"), SettlementPlanPages) && SettlementPlanPages)
		{
			TSet<FString> SeenBuildingIds;
			for (const TSharedPtr<FJsonValue>& PageValue : *SettlementPlanPages)
			{
				TSharedPtr<FJsonObject> PageObject = PageValue.IsValid() ? PageValue->AsObject() : nullptr;
				if (!PageObject.IsValid())
				{
					continue;
				}
				if (const TSharedPtr<FJsonObject>* NestedPlan = nullptr; PageObject->TryGetObjectField(TEXT("settlement_plan"), NestedPlan) && NestedPlan && NestedPlan->IsValid())
				{
					PageObject = *NestedPlan;
				}
				if (!SettlementPlan.IsValid())
				{
					SettlementPlan = PageObject;
				}
				const TArray<TSharedPtr<FJsonValue>>* PageBuildings = nullptr;
				if (!PageObject->TryGetArrayField(TEXT("buildings"), PageBuildings) || !PageBuildings)
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& BuildingValue : *PageBuildings)
				{
					const TSharedPtr<FJsonObject> BuildingObject = BuildingValue.IsValid() ? BuildingValue->AsObject() : nullptr;
					FString BuildingId;
					ReadStringFieldAny(BuildingObject, { TEXT("id"), TEXT("building_id") }, BuildingId);
					if (!BuildingId.IsEmpty() && SeenBuildingIds.Contains(BuildingId))
					{
						continue;
					}
					if (!BuildingId.IsEmpty())
					{
						SeenBuildingIds.Add(BuildingId);
					}
					AggregatedBuildings.Add(BuildingValue);
				}
			}
			Buildings = &AggregatedBuildings;
		}
		if ((!Buildings || Buildings->IsEmpty()) && (!SettlementPlan.IsValid() || !SettlementPlan->TryGetArrayField(TEXT("buildings"), Buildings) || !Buildings))
		{
			if (!Arguments->TryGetArrayField(TEXT("buildings"), Buildings) || !Buildings)
			{
				Out->SetBoolField(TEXT("ok"), false);
				SololmcpError::MissingParam(Out, TEXT("settlement_plan_pages|settlement_plan.buildings"));
				Error = TEXT("Missing settlement_plan_pages, settlement_plan.buildings, or buildings.");
				return false;
			}
		}
		if (Buildings->IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("status"), TEXT("city_batch_plan_failed_empty_buildings"));
			Error = TEXT("No buildings were supplied for city batch planning.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		FString SettlementId;
		ReadStringFieldAny(SettlementPlan, { TEXT("id"), TEXT("settlement_id"), TEXT("name") }, SettlementId);
		ReadStringFieldAny(Arguments, { TEXT("settlement_id") }, SettlementId);
		SettlementId = MakeArchitectureIdToken(SettlementId, TEXT("settlement"));

		FString PlanId;
		ReadStringFieldAny(Arguments, { TEXT("plan_id"), TEXT("batch_plan_id") }, PlanId);
		PlanId = MakeArchitectureIdToken(PlanId, FString::Printf(TEXT("%s_city_batch_plan"), *SettlementId));

		double MaxBuildingsValue = 64.0;
		ReadNumberFieldAny(Arguments, { TEXT("max_buildings_per_batch") }, MaxBuildingsValue);
		const int32 MaxBuildingsPerBatch = FMath::Clamp(FMath::RoundToInt(MaxBuildingsValue), 1, 256);

		double MaxModulesValue = 1200.0;
		ReadNumberFieldAny(Arguments, { TEXT("max_modules_per_batch") }, MaxModulesValue);
		const int32 MaxModulesPerBatch = FMath::Clamp(FMath::RoundToInt(MaxModulesValue), 32, 10000);

		double WriteLaneValue = 4.0;
		ReadNumberFieldAny(Arguments, { TEXT("max_parallel_write_batches"), TEXT("write_lane_limit") }, WriteLaneValue);
		const int32 WriteLaneLimit = FMath::Clamp(FMath::RoundToInt(WriteLaneValue), 1, 16);

		TArray<TSharedPtr<FJsonValue>> Batches;
		TArray<TSharedPtr<FJsonValue>> ExecutionGroups;
		TArray<FString> Warnings;
		int32 BatchStart = 0;
		int32 CurrentBatchModules = 0;
		int32 CurrentBatchBuildings = 0;
		int32 TotalEstimatedModules = 0;
		int32 MaxObservedBuildings = 0;
		int32 MaxObservedModules = 0;

		auto FlushBatch = [&](int32 ExclusiveEnd)
		{
			if (CurrentBatchBuildings <= 0)
			{
				return;
			}
			const int32 BatchIndex = Batches.Num();
			const FString BatchId = FString::Printf(TEXT("%s_batch_%03d"), *PlanId, BatchIndex + 1);
			TSharedRef<FJsonObject> Batch = MakeShared<FJsonObject>();
			Batch->SetStringField(TEXT("batch_id"), BatchId);
			Batch->SetStringField(TEXT("settlement_id"), SettlementId);
			Batch->SetStringField(TEXT("assembly_id"), FString::Printf(TEXT("%s_assembly_%03d"), *SettlementId, BatchIndex + 1));
			Batch->SetNumberField(TEXT("building_start_index"), BatchStart);
			Batch->SetNumberField(TEXT("building_end_index_exclusive"), ExclusiveEnd);
			Batch->SetNumberField(TEXT("building_count"), CurrentBatchBuildings);
			Batch->SetNumberField(TEXT("estimated_module_count"), CurrentBatchModules);
			Batch->SetNumberField(TEXT("execution_group"), BatchIndex / WriteLaneLimit);
			Batch->SetStringField(TEXT("lane"), TEXT("ue_editor_write"));
			Batch->SetArrayField(TEXT("tools_required"), StringArrayJson({
				TEXT("architecture_modular_assembly_plan"),
				TEXT("architecture_assembly_execute"),
				TEXT("architecture_assembly_readback"),
				TEXT("architecture_collision_generate"),
				TEXT("architecture_collision_audit"),
				TEXT("architecture_interior_nav_build"),
				TEXT("architecture_navlink_generate"),
				TEXT("architecture_reachability_audit"),
				TEXT("architecture_navmesh_path_sample")
			}));
			Batch->SetArrayField(TEXT("hard_gates"), StringArrayJson({
				TEXT("target_guard"),
				TEXT("resource_lock"),
				TEXT("assembly_readback"),
				TEXT("collision_audit"),
				TEXT("reachability_audit"),
				TEXT("navmesh_path_sample")
			}));
			Batches.Add(MakeShared<FJsonValueObject>(Batch));
			MaxObservedBuildings = FMath::Max(MaxObservedBuildings, CurrentBatchBuildings);
			MaxObservedModules = FMath::Max(MaxObservedModules, CurrentBatchModules);
			BatchStart = ExclusiveEnd;
			CurrentBatchModules = 0;
			CurrentBatchBuildings = 0;
		};

		for (int32 BuildingIndex = 0; BuildingIndex < Buildings->Num(); ++BuildingIndex)
		{
			const TSharedPtr<FJsonObject> Building = (*Buildings)[BuildingIndex].IsValid() ? (*Buildings)[BuildingIndex]->AsObject() : nullptr;
			const int32 EstimatedModules = EstimateArchitectureModulesForBuilding(Building);
			if (EstimatedModules > MaxModulesPerBatch)
			{
				Warnings.Add(FString::Printf(TEXT("building_%03d_estimated_modules_exceed_batch_module_guard"), BuildingIndex + 1));
			}
			const bool bWouldExceedBuildingGuard = CurrentBatchBuildings >= MaxBuildingsPerBatch;
			const bool bWouldExceedModuleGuard = CurrentBatchBuildings > 0 && (CurrentBatchModules + EstimatedModules) > MaxModulesPerBatch;
			if (bWouldExceedBuildingGuard || bWouldExceedModuleGuard)
			{
				FlushBatch(BuildingIndex);
			}
			CurrentBatchBuildings += 1;
			CurrentBatchModules += EstimatedModules;
			TotalEstimatedModules += EstimatedModules;
		}
		FlushBatch(Buildings->Num());

		const int32 GroupCount = FMath::DivideAndRoundUp(Batches.Num(), FMath::Max(1, WriteLaneLimit));
		for (int32 GroupIndex = 0; GroupIndex < GroupCount; ++GroupIndex)
		{
			TSharedRef<FJsonObject> Group = MakeShared<FJsonObject>();
			Group->SetNumberField(TEXT("group_index"), GroupIndex);
			Group->SetStringField(TEXT("lane"), TEXT("ue_editor_write"));
			Group->SetNumberField(TEXT("max_parallel_batches"), WriteLaneLimit);
			Group->SetStringField(TEXT("policy"), TEXT("parallel_plan_readback_then_locked_mutating_writes"));
			ExecutionGroups.Add(MakeShared<FJsonValueObject>(Group));
		}

		TSharedRef<FJsonObject> BatchPlan = MakeShared<FJsonObject>();
		BatchPlan->SetStringField(TEXT("plan_id"), PlanId);
		BatchPlan->SetStringField(TEXT("settlement_id"), SettlementId);
		BatchPlan->SetNumberField(TEXT("building_count"), Buildings->Num());
		BatchPlan->SetNumberField(TEXT("estimated_module_count"), TotalEstimatedModules);
		BatchPlan->SetNumberField(TEXT("batch_count"), Batches.Num());
		BatchPlan->SetNumberField(TEXT("max_buildings_per_batch"), MaxBuildingsPerBatch);
		BatchPlan->SetNumberField(TEXT("max_modules_per_batch"), MaxModulesPerBatch);
		BatchPlan->SetNumberField(TEXT("max_observed_buildings_per_batch"), MaxObservedBuildings);
		BatchPlan->SetNumberField(TEXT("max_observed_modules_per_batch"), MaxObservedModules);
		BatchPlan->SetNumberField(TEXT("max_parallel_write_batches"), WriteLaneLimit);
		BatchPlan->SetArrayField(TEXT("batches"), Batches);
		BatchPlan->SetArrayField(TEXT("execution_groups"), ExecutionGroups);
		BatchPlan->SetArrayField(TEXT("post_batch_followup_tools"), StringArrayJson({
			TEXT("architecture_deployment_metadata_apply"),
			TEXT("world_create_data_layer_actor_membership_apply"),
			TEXT("architecture_hlod_build_dispatch"),
			TEXT("world_create_hlod_job_poll"),
			TEXT("world_create_save_validate_fast"),
			TEXT("world_create_reload_verify"),
			TEXT("editor_screenshot_viewport"),
			TEXT("architecture_city_scale_gate_validate")
		}));

		const bool bOk = !Batches.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("city_batch_plan_ready") : TEXT("city_batch_plan_failed"));
		Out->SetStringField(TEXT("plan_id"), PlanId);
		Out->SetObjectField(TEXT("city_batch_plan"), BatchPlan);
		Out->SetNumberField(TEXT("building_count"), Buildings->Num());
		Out->SetNumberField(TEXT("estimated_module_count"), TotalEstimatedModules);
		Out->SetNumberField(TEXT("batch_count"), Batches.Num());
		Out->SetArrayField(TEXT("warnings"), StringArrayJson(Warnings));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%s:%d:%d:%d:%d"), *PlanId, *SettlementId, Buildings->Num(), TotalEstimatedModules, Batches.Num(), MaxBuildingsPerBatch)));
		Summary = FString::Printf(TEXT("architecture_city_batch_plan split %d buildings into %d batches."), Buildings->Num(), Batches.Num());
		return bOk;
	}

	static bool RunArchitectureSettlementFootprintAudit(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_settlement_footprint_audit"));
		TSharedPtr<FJsonObject> SettlementPlan = ResolveNamedPlanObject(Arguments, TEXT("settlement_plan"));
		const TArray<TSharedPtr<FJsonValue>>* Buildings = nullptr;
		if (!SettlementPlan.IsValid() || !SettlementPlan->TryGetArrayField(TEXT("buildings"), Buildings) || !Buildings)
		{
			if (!Arguments->TryGetArrayField(TEXT("buildings"), Buildings) || !Buildings)
			{
				Out->SetBoolField(TEXT("ok"), false);
				SololmcpError::MissingParam(Out, TEXT("settlement_plan.buildings"));
				Error = TEXT("Missing settlement_plan.buildings or buildings.");
				Out->SetStringField(TEXT("error"), Error);
				return false;
			}
		}

		bool bAllowOverlap = false;
		Arguments->TryGetBoolField(TEXT("allow_overlap"), bAllowOverlap);
		double MinSpacingCm = 100.0;
		ReadNumberFieldAny(Arguments, { TEXT("min_spacing_cm"), TEXT("min_building_spacing_cm") }, MinSpacingCm);

		struct FFootprintBox
		{
			FString Id;
			FVector Center = FVector::ZeroVector;
			double HalfWidth = 0.0;
			double HalfDepth = 0.0;
		};

		TArray<FFootprintBox> Boxes;
		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<FString> Failures;
		for (int32 Index = 0; Index < Buildings->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Building = (*Buildings)[Index].IsValid() ? (*Buildings)[Index]->AsObject() : nullptr;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), Index);
			FString BuildingId;
			ReadStringFieldAny(Building, { TEXT("id"), TEXT("building_id"), TEXT("name") }, BuildingId);
			BuildingId = MakeArchitectureIdToken(BuildingId, FString::Printf(TEXT("building_%03d"), Index + 1));
			Row->SetStringField(TEXT("building_id"), BuildingId);

			FVector Center = FVector::ZeroVector;
			const bool bHasLocation = TryReadVectorField(Building, TEXT("location_cm"), Center)
				|| TryReadVectorField(Building, TEXT("location"), Center)
				|| TryReadVectorField(Building, TEXT("center_cm"), Center)
				|| TryReadVectorField(Building, TEXT("center"), Center);
			const double WidthCm = ReadNestedNumberOrDefault(Building, TEXT("footprint_cm"), { TEXT("width_cm"), TEXT("width") }, 900.0);
			const double DepthCm = ReadNestedNumberOrDefault(Building, TEXT("footprint_cm"), { TEXT("depth_cm"), TEXT("depth") }, 900.0);
			TArray<FString> RowFailures;
			if (!bHasLocation)
			{
				RowFailures.Add(TEXT("missing_location"));
			}
			if (WidthCm <= 0.0 || DepthCm <= 0.0)
			{
				RowFailures.Add(TEXT("invalid_footprint_size"));
			}
			Row->SetObjectField(TEXT("center_cm"), VectorJson(Center));
			Row->SetNumberField(TEXT("width_cm"), WidthCm);
			Row->SetNumberField(TEXT("depth_cm"), DepthCm);
			Row->SetArrayField(TEXT("failures"), StringArrayJson(RowFailures));
			for (const FString& Failure : RowFailures)
			{
				Failures.Add(FString::Printf(TEXT("%s:%s"), *BuildingId, *Failure));
			}
			if (RowFailures.IsEmpty())
			{
				FFootprintBox Box;
				Box.Id = BuildingId;
				Box.Center = Center;
				Box.HalfWidth = WidthCm * 0.5;
				Box.HalfDepth = DepthCm * 0.5;
				Boxes.Add(Box);
			}
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}

		TArray<TSharedPtr<FJsonValue>> PairRows;
		for (int32 LeftIndex = 0; LeftIndex < Boxes.Num(); ++LeftIndex)
		{
			for (int32 RightIndex = LeftIndex + 1; RightIndex < Boxes.Num(); ++RightIndex)
			{
				const FFootprintBox& Left = Boxes[LeftIndex];
				const FFootprintBox& Right = Boxes[RightIndex];
				const double Dx = FMath::Abs(Left.Center.X - Right.Center.X);
				const double Dy = FMath::Abs(Left.Center.Y - Right.Center.Y);
				const bool bOverlap = Dx < (Left.HalfWidth + Right.HalfWidth) && Dy < (Left.HalfDepth + Right.HalfDepth);
				const double CenterDistance = FVector::Dist2D(Left.Center, Right.Center);
				const bool bTooClose = !bOverlap && CenterDistance < MinSpacingCm;
				if ((!bAllowOverlap && bOverlap) || bTooClose)
				{
					TSharedRef<FJsonObject> Pair = MakeShared<FJsonObject>();
					Pair->SetStringField(TEXT("left"), Left.Id);
					Pair->SetStringField(TEXT("right"), Right.Id);
					Pair->SetBoolField(TEXT("overlap"), bOverlap);
					Pair->SetBoolField(TEXT("too_close"), bTooClose);
					Pair->SetNumberField(TEXT("center_distance_cm"), CenterDistance);
					PairRows.Add(MakeShared<FJsonValueObject>(Pair));
					Failures.Add(FString::Printf(TEXT("%s:%s:%s"), *Left.Id, *Right.Id, bOverlap ? TEXT("footprint_overlap") : TEXT("spacing_below_minimum")));
				}
			}
		}

		const bool bOk = Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("settlement_footprint_valid") : TEXT("settlement_footprint_invalid"));
		Out->SetBoolField(TEXT("allow_overlap"), bAllowOverlap);
		Out->SetNumberField(TEXT("building_count"), Buildings->Num());
		Out->SetNumberField(TEXT("validated_footprint_count"), Boxes.Num());
		Out->SetNumberField(TEXT("min_spacing_cm"), MinSpacingCm);
		Out->SetArrayField(TEXT("building_results"), Rows);
		Out->SetArrayField(TEXT("conflict_pairs"), PairRows);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%d:%d"), Buildings->Num(), Boxes.Num(), Failures.Num())));
		Summary = FString::Printf(TEXT("architecture_settlement_footprint_audit checked %d building footprints."), Buildings->Num());
		if (!bOk)
		{
			Error = TEXT("Architecture settlement footprint audit failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunBridgePassabilityAudit(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("bridge_passability_audit"));
		TSharedPtr<FJsonObject> Plan = ResolveNamedPlanObject(Arguments, TEXT("bridge_plan"));

		FString BridgeId;
		ReadStringFieldAny(Plan, { TEXT("id"), TEXT("bridge_id"), TEXT("name") }, BridgeId);
		if (BridgeId.IsEmpty())
		{
			BridgeId = TEXT("bridge");
		}
		FString BridgeKind = TEXT("generic");
		ReadStringFieldAny(Plan, { TEXT("kind"), TEXT("bridge_kind"), TEXT("type") }, BridgeKind);
		BridgeKind = BridgeKind.ToLower();

		double WidthCm = 0.0;
		ReadNumberFieldAny(Plan, { TEXT("deck_width_cm"), TEXT("width_cm"), TEXT("passage_width_cm") }, WidthCm);
		double MinWidthCm = 180.0;
		ReadNumberFieldAny(Plan, { TEXT("min_width_cm"), TEXT("min_passage_width_cm") }, MinWidthCm);

		double SlopeDeg = 0.0;
		ReadNumberFieldAny(Plan, { TEXT("slope_degrees"), TEXT("max_segment_slope_degrees") }, SlopeDeg);
		double MaxSlopeDeg = 18.0;
		ReadNumberFieldAny(Plan, { TEXT("max_slope_degrees"), TEXT("allowed_slope_degrees") }, MaxSlopeDeg);

		double ClearanceCm = 0.0;
		ReadNumberFieldAny(Plan, { TEXT("clearance_height_cm"), TEXT("min_clearance_height_cm") }, ClearanceCm);
		double RequiredClearanceCm = 0.0;
		ReadNumberFieldAny(Plan, { TEXT("required_clearance_height_cm") }, RequiredClearanceCm);

		const TArray<TSharedPtr<FJsonValue>>* Endpoints = nullptr;
		Plan->TryGetArrayField(TEXT("endpoints"), Endpoints);
		const TArray<TSharedPtr<FJsonValue>>* RoadNodes = nullptr;
		if (!Plan->TryGetArrayField(TEXT("road_nodes"), RoadNodes))
		{
			Arguments->TryGetArrayField(TEXT("road_nodes"), RoadNodes);
		}
		const TArray<TSharedPtr<FJsonValue>>* NavLinks = nullptr;
		if (!Plan->TryGetArrayField(TEXT("nav_links"), NavLinks))
		{
			Plan->TryGetArrayField(TEXT("links"), NavLinks);
		}

		TSet<FString> RoadIds;
		CollectStringIdsFromObjects(RoadNodes, { TEXT("id"), TEXT("road_id"), TEXT("node_id") }, RoadIds);

		int32 ConnectedEndpointCount = 0;
		TArray<TSharedPtr<FJsonValue>> EndpointRows;
		if (Endpoints)
		{
			for (int32 Index = 0; Index < Endpoints->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Endpoint = (*Endpoints)[Index].IsValid() ? (*Endpoints)[Index]->AsObject() : nullptr;
				FString EndpointId;
				ReadStringFieldAny(Endpoint, { TEXT("id"), TEXT("endpoint_id"), TEXT("name") }, EndpointId);
				if (EndpointId.IsEmpty())
				{
					EndpointId = FString::Printf(TEXT("endpoint_%03d"), Index);
				}
				FString ConnectedRoad;
				ReadStringFieldAny(Endpoint, { TEXT("connected_road_id"), TEXT("road_id"), TEXT("connected_to") }, ConnectedRoad);
				FVector Location = FVector::ZeroVector;
				const bool bHasLocation = TryReadVectorField(Endpoint, TEXT("location_cm"), Location) || TryReadVectorField(Endpoint, TEXT("location"), Location);
				const bool bConnected = (!ConnectedRoad.IsEmpty() && (RoadIds.IsEmpty() || RoadIds.Contains(ConnectedRoad))) || bHasLocation;
				if (bConnected)
				{
					++ConnectedEndpointCount;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("endpoint_id"), EndpointId);
				Row->SetStringField(TEXT("connected_road_id"), ConnectedRoad);
				Row->SetBoolField(TEXT("has_location"), bHasLocation);
				Row->SetBoolField(TEXT("connected"), bConnected);
				EndpointRows.Add(MakeShared<FJsonValueObject>(Row));
			}
		}

		int32 NavLinkCount = NavLinks ? NavLinks->Num() : 0;
		if (NavLinkCount <= 0 && Endpoints && Endpoints->Num() >= 2)
		{
			NavLinkCount = 1;
		}

		bool bDrawbridgeOpen = true;
		FString State;
		if (ReadStringFieldAny(Plan, { TEXT("state"), TEXT("gate_state"), TEXT("drawbridge_state") }, State))
		{
			State = State.ToLower();
			bDrawbridgeOpen = State != TEXT("closed") && State != TEXT("raised") && State != TEXT("locked");
		}
		bool bRequirePassableWhenClosed = false;
		Arguments->TryGetBoolField(TEXT("require_passable_when_closed"), bRequirePassableWhenClosed);

		TArray<FString> Failures;
		if (WidthCm <= 0.0)
		{
			Failures.Add(TEXT("missing_bridge_width_cm"));
		}
		else if (WidthCm < MinWidthCm)
		{
			Failures.Add(TEXT("bridge_width_below_minimum"));
		}
		if (SlopeDeg > MaxSlopeDeg)
		{
			Failures.Add(TEXT("bridge_slope_above_maximum"));
		}
		if (RequiredClearanceCm > 0.0 && ClearanceCm < RequiredClearanceCm)
		{
			Failures.Add(TEXT("bridge_clearance_below_required"));
		}
		if (!Endpoints || Endpoints->Num() < 2)
		{
			Failures.Add(TEXT("missing_two_bridge_endpoints"));
		}
		else if (ConnectedEndpointCount < 2)
		{
			Failures.Add(TEXT("bridge_endpoints_not_connected"));
		}
		if (NavLinkCount <= 0)
		{
			Failures.Add(TEXT("missing_bridge_nav_link_or_endpoint_pair"));
		}
		if (BridgeKind.Contains(TEXT("draw")) && !bDrawbridgeOpen && !bRequirePassableWhenClosed)
		{
			Failures.Add(TEXT("drawbridge_closed"));
		}

		const bool bOk = Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("bridge_passability_valid") : TEXT("bridge_passability_failed"));
		Out->SetStringField(TEXT("bridge_id"), BridgeId);
		Out->SetStringField(TEXT("bridge_kind"), BridgeKind);
		Out->SetNumberField(TEXT("deck_width_cm"), WidthCm);
		Out->SetNumberField(TEXT("min_width_cm"), MinWidthCm);
		Out->SetNumberField(TEXT("slope_degrees"), SlopeDeg);
		Out->SetNumberField(TEXT("max_slope_degrees"), MaxSlopeDeg);
		Out->SetNumberField(TEXT("endpoint_count"), Endpoints ? Endpoints->Num() : 0);
		Out->SetNumberField(TEXT("connected_endpoint_count"), ConnectedEndpointCount);
		Out->SetNumberField(TEXT("nav_link_count"), NavLinkCount);
		Out->SetBoolField(TEXT("drawbridge_open"), bDrawbridgeOpen);
		Out->SetArrayField(TEXT("endpoints"), EndpointRows);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%s:%f:%f:%d:%d:%d"), *BridgeId, *BridgeKind, WidthCm, SlopeDeg, ConnectedEndpointCount, NavLinkCount, Failures.Num())));
		Summary = FString::Printf(TEXT("bridge_passability_audit %s endpoints=%d/%d nav_links=%d."), bOk ? TEXT("passed") : TEXT("failed"), ConnectedEndpointCount, Endpoints ? Endpoints->Num() : 0, NavLinkCount);
		if (!bOk)
		{
			Error = TEXT("Bridge passability audit failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunFortressGateNavValidate(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("fortress_gate_nav_validate"));
		TSharedPtr<FJsonObject> Plan = ResolveNamedPlanObject(Arguments, TEXT("fortress_plan"));

		const TArray<TSharedPtr<FJsonValue>>* Gates = nullptr;
		Plan->TryGetArrayField(TEXT("gates"), Gates);
		const TArray<TSharedPtr<FJsonValue>>* NavLinks = nullptr;
		if (!Plan->TryGetArrayField(TEXT("nav_links"), NavLinks))
		{
			Plan->TryGetArrayField(TEXT("links"), NavLinks);
		}
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		Plan->TryGetArrayField(TEXT("nodes"), Nodes);

		TSet<FString> NodeIds;
		CollectStringIdsFromObjects(Nodes, { TEXT("id"), TEXT("node_id"), TEXT("name") }, NodeIds);
		TSet<FString> LinkedPairs;
		if (NavLinks)
		{
			for (const TSharedPtr<FJsonValue>& Value : *NavLinks)
			{
				const TSharedPtr<FJsonObject> Link = Value.IsValid() ? Value->AsObject() : nullptr;
				FString From;
				FString To;
				ReadStringFieldAny(Link, { TEXT("from"), TEXT("from_id"), TEXT("left_node_id"), TEXT("exterior_node_id") }, From);
				ReadStringFieldAny(Link, { TEXT("to"), TEXT("to_id"), TEXT("right_node_id"), TEXT("interior_node_id") }, To);
				if (!From.IsEmpty() && !To.IsEmpty())
				{
					LinkedPairs.Add(From + TEXT("->") + To);
					LinkedPairs.Add(To + TEXT("->") + From);
				}
			}
		}

		bool bRequireOpenPath = true;
		Arguments->TryGetBoolField(TEXT("require_open_path"), bRequireOpenPath);
		bool bAllowClosedGateByDesign = false;
		Arguments->TryGetBoolField(TEXT("allow_closed_gate"), bAllowClosedGateByDesign);

		TArray<TSharedPtr<FJsonValue>> GateRows;
		TArray<FString> Failures;
		int32 OpenGateCount = 0;
		int32 ConnectedOpenGateCount = 0;
		if (!Gates || Gates->IsEmpty())
		{
			Failures.Add(TEXT("missing_gates"));
		}
		else
		{
			for (int32 Index = 0; Index < Gates->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Gate = (*Gates)[Index].IsValid() ? (*Gates)[Index]->AsObject() : nullptr;
				FString GateId;
				ReadStringFieldAny(Gate, { TEXT("id"), TEXT("gate_id"), TEXT("name") }, GateId);
				if (GateId.IsEmpty())
				{
					GateId = FString::Printf(TEXT("gate_%03d"), Index);
				}
				FString State = TEXT("open");
				ReadStringFieldAny(Gate, { TEXT("state"), TEXT("gate_state") }, State);
				State = State.ToLower();
				const bool bOpen = State != TEXT("closed") && State != TEXT("locked") && State != TEXT("destroyed");
				if (bOpen)
				{
					++OpenGateCount;
				}

				FString Exterior;
				FString Interior;
				ReadStringFieldAny(Gate, { TEXT("exterior_node_id"), TEXT("outside_node_id"), TEXT("from") }, Exterior);
				ReadStringFieldAny(Gate, { TEXT("interior_node_id"), TEXT("inside_node_id"), TEXT("to") }, Interior);
				const bool bNodesKnown = (NodeIds.IsEmpty() || (NodeIds.Contains(Exterior) && NodeIds.Contains(Interior)));
				const bool bHasExplicitConnection = !Exterior.IsEmpty() && !Interior.IsEmpty() && LinkedPairs.Contains(Exterior + TEXT("->") + Interior);
				const bool bHasInlineConnection = JsonObjectContainsAnyToken(Gate, { TEXT("nav_link"), TEXT("connected"), TEXT("passable") });
				const bool bConnected = bNodesKnown && (bHasExplicitConnection || bHasInlineConnection);
				if (bOpen && bConnected)
				{
					++ConnectedOpenGateCount;
				}
				if (bRequireOpenPath && bOpen && !bConnected)
				{
					Failures.Add(FString::Printf(TEXT("gate[%d]_%s_missing_open_nav_connection"), Index, *GateId));
				}
				if (!bOpen && !bAllowClosedGateByDesign)
				{
					Failures.Add(FString::Printf(TEXT("gate[%d]_%s_closed_without_allowance"), Index, *GateId));
				}

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("gate_id"), GateId);
				Row->SetStringField(TEXT("state"), State);
				Row->SetStringField(TEXT("exterior_node_id"), Exterior);
				Row->SetStringField(TEXT("interior_node_id"), Interior);
				Row->SetBoolField(TEXT("open"), bOpen);
				Row->SetBoolField(TEXT("nodes_known"), bNodesKnown);
				Row->SetBoolField(TEXT("connected"), bConnected);
				GateRows.Add(MakeShared<FJsonValueObject>(Row));
			}
		}

		if (bRequireOpenPath && OpenGateCount <= 0)
		{
			Failures.Add(TEXT("no_open_gate_for_required_path"));
		}

		const bool bOk = Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("fortress_gate_nav_valid") : TEXT("fortress_gate_nav_failed"));
		Out->SetNumberField(TEXT("gate_count"), Gates ? Gates->Num() : 0);
		Out->SetNumberField(TEXT("open_gate_count"), OpenGateCount);
		Out->SetNumberField(TEXT("connected_open_gate_count"), ConnectedOpenGateCount);
		Out->SetNumberField(TEXT("node_count"), NodeIds.Num());
		Out->SetNumberField(TEXT("nav_link_count"), NavLinks ? NavLinks->Num() : 0);
		Out->SetBoolField(TEXT("require_open_path"), bRequireOpenPath);
		Out->SetBoolField(TEXT("allow_closed_gate"), bAllowClosedGateByDesign);
		Out->SetArrayField(TEXT("gates"), GateRows);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%d:%d:%d:%d"), Gates ? Gates->Num() : 0, OpenGateCount, ConnectedOpenGateCount, NavLinks ? NavLinks->Num() : 0, Failures.Num())));
		Summary = FString::Printf(TEXT("fortress_gate_nav_validate %s open=%d connected=%d."), bOk ? TEXT("passed") : TEXT("failed"), OpenGateCount, ConnectedOpenGateCount);
		if (!bOk)
		{
			Error = TEXT("Fortress gate navigation validation failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool TryResolveInteriorGraph(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedPtr<FJsonObject>& OutGraph,
		const TArray<TSharedPtr<FJsonValue>>*& OutNodes,
		const TArray<TSharedPtr<FJsonValue>>*& OutLinks)
	{
		OutGraph = Arguments;
		if (const TSharedPtr<FJsonObject>* GraphPtr = nullptr; Arguments->TryGetObjectField(TEXT("interior_nav_graph"), GraphPtr) && GraphPtr && GraphPtr->IsValid())
		{
			OutGraph = *GraphPtr;
		}
		OutNodes = nullptr;
		OutLinks = nullptr;
		return OutGraph.IsValid()
			&& OutGraph->TryGetArrayField(TEXT("nodes"), OutNodes) && OutNodes
			&& OutGraph->TryGetArrayField(TEXT("links"), OutLinks) && OutLinks;
	}

	static FString NodeIdFromJson(const TSharedPtr<FJsonObject>& Node, int32 Index)
	{
		FString Id;
		if (!Node.IsValid() || (!Node->TryGetStringField(TEXT("id"), Id) && !Node->TryGetStringField(TEXT("node_id"), Id)) || Id.TrimStartAndEnd().IsEmpty())
		{
			Id = FString::Printf(TEXT("node_%03d"), Index);
		}
		return Id;
	}

	static bool BuildInteriorNodeMap(
		const TArray<TSharedPtr<FJsonValue>>* Nodes,
		TMap<FString, FVector>& OutLocations,
		TSet<FString>& OutNodeIds,
		int32& OutEntranceNodeCount)
	{
		OutLocations.Reset();
		OutNodeIds.Reset();
		OutEntranceNodeCount = 0;
		if (!Nodes)
		{
			return false;
		}

		for (int32 Index = 0; Index < Nodes->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Node = (*Nodes)[Index].IsValid() ? (*Nodes)[Index]->AsObject() : nullptr;
			if (!Node.IsValid())
			{
				continue;
			}
			const FString Id = NodeIdFromJson(Node, Index);
			OutNodeIds.Add(Id);

			FString Kind;
			Node->TryGetStringField(TEXT("kind"), Kind);
			if (Kind.IsEmpty())
			{
				Node->TryGetStringField(TEXT("node_kind"), Kind);
			}
			if (Kind.Equals(TEXT("entrance"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("door"), ESearchCase::IgnoreCase))
			{
				++OutEntranceNodeCount;
			}

			FVector Location = FVector::ZeroVector;
			if (TryReadVectorField(Node, TEXT("location_cm"), Location) || TryReadVectorField(Node, TEXT("location"), Location))
			{
				OutLocations.Add(Id, Location);
			}
		}
		return !OutNodeIds.IsEmpty();
	}

	static TSharedRef<FJsonObject> NavLinkReceiptJson(
		const FString& LinkId,
		const FVector& Left,
		const FVector& Right,
		const ANavLinkProxy* Proxy)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("link_id"), LinkId);
		Obj->SetObjectField(TEXT("left_cm"), VectorJson(Left));
		Obj->SetObjectField(TEXT("right_cm"), VectorJson(Right));
		Obj->SetStringField(TEXT("actor_label"), IsValid(Proxy) ? Proxy->GetActorLabel() : FString());
		Obj->SetStringField(TEXT("actor_path"), IsValid(Proxy) ? Proxy->GetPathName() : FString());
		return Obj;
	}

	static void DirtyNavigationForActor(UWorld* World, AActor* Actor, bool& bDirtyQueued)
	{
		if (!World || !IsValid(Actor))
		{
			return;
		}
		if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
		{
			UNavigationSystemV1::UpdateActorAndComponentsInNavOctree(*Actor);
			const FBox Bounds = Actor->GetComponentsBoundingBox(true).ExpandBy(100.0);
			if (Bounds.IsValid)
			{
				NavSys->AddDirtyArea(Bounds, ENavigationDirtyFlag::All, TEXT("SOMOLArchitectureNav"));
				bDirtyQueued = true;
			}
		}
	}

	static bool RunArchitectureInteriorNavBuild(
		const FSololmcpToolExecutionContext& Context,
		const TArray<FString>& RequiredReceipts,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_interior_nav_build"));
		Out->SetArrayField(TEXT("required_receipts"), StringArrayJson(RequiredReceipts));
		Out->SetBoolField(TEXT("mutating"), true);

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("interior_nav_writer_ready_dry_run"));
			Summary = TEXT("architecture_interior_nav_build dry-run contract returned; pass execute=true to update nav relevance and dirty navigation.");
			return true;
		}

		TSharedPtr<FJsonObject> Graph;
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
		if (!TryResolveInteriorGraph(Arguments, Graph, Nodes, Links) || !Nodes || !Links)
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("interior_nav_graph.nodes/links"));
			Error = TEXT("Missing interior_nav_graph.nodes/links.");
			return false;
		}

		TMap<FString, FVector> NodeLocations;
		TSet<FString> NodeIds;
		int32 EntranceNodeCount = 0;
		BuildInteriorNodeMap(Nodes, NodeLocations, NodeIds, EntranceNodeCount);
		if (NodeIds.IsEmpty() || EntranceNodeCount <= 0)
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("status"), TEXT("interior_nav_graph_invalid"));
			Out->SetNumberField(TEXT("interior_node_count"), NodeIds.Num());
			Out->SetNumberField(TEXT("entrance_node_count"), EntranceNodeCount);
			Error = TEXT("Interior navigation graph requires at least one node and one entrance/door node.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		UWorld* World = GetEditorWorld(Context, Out, Error);
		if (!World)
		{
			return false;
		}

		TArray<AActor*> Actors;
		FString AssemblyId;
		ResolveArchitectureActors(World, Arguments, Actors, AssemblyId);
		int32 ComponentCount = 0;
		int32 NavRelevantComponentCount = 0;
		bool bDirtyQueued = false;
		for (AActor* Actor : Actors)
		{
			if (!IsValid(Actor))
			{
				continue;
			}
			Actor->Modify();
			Actor->Tags.AddUnique(FName(TEXT("SOMOLArchitectureInteriorNav")));
			TArray<UStaticMeshComponent*> Components;
			Actor->GetComponents<UStaticMeshComponent>(Components);
			for (UStaticMeshComponent* Component : Components)
			{
				if (!Component)
				{
					continue;
				}
				++ComponentCount;
				Component->Modify();
				Component->SetCanEverAffectNavigation(true);
				Component->MarkRenderStateDirty();
				if (Component->CanEverAffectNavigation())
				{
					++NavRelevantComponentCount;
				}
			}
			DirtyNavigationForActor(World, Actor, bDirtyQueued);
			Actor->MarkPackageDirty();
		}

		bool bRebuildNavigation = false;
		Arguments->TryGetBoolField(TEXT("rebuild_navigation"), bRebuildNavigation);
		bool bRebuildTriggered = false;
		if (bRebuildNavigation)
		{
			if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
			{
				NavSys->Build();
				bRebuildTriggered = true;
			}
		}

		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("interior_nav_build_ready"));
		Out->SetStringField(TEXT("assembly_id"), AssemblyId);
		Out->SetNumberField(TEXT("interior_node_count"), NodeIds.Num());
		Out->SetNumberField(TEXT("interior_link_count"), Links->Num());
		Out->SetNumberField(TEXT("entrance_node_count"), EntranceNodeCount);
		Out->SetNumberField(TEXT("actor_count"), Actors.Num());
		Out->SetNumberField(TEXT("static_mesh_component_count"), ComponentCount);
		Out->SetNumberField(TEXT("nav_relevant_component_count"), NavRelevantComponentCount);
		Out->SetBoolField(TEXT("navigation_dirty_area_queued"), bDirtyQueued);
		Out->SetBoolField(TEXT("navigation_rebuild_triggered"), bRebuildTriggered);
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%d:%d:%d:%d"), NodeIds.Num(), Links->Num(), EntranceNodeCount, Actors.Num(), NavRelevantComponentCount)));
		Summary = FString::Printf(TEXT("architecture_interior_nav_build nodes=%d links=%d actors=%d nav_components=%d."), NodeIds.Num(), Links->Num(), Actors.Num(), NavRelevantComponentCount);
		return true;
	}

	static bool ResolveNavLinkEndpoints(
		const TSharedPtr<FJsonObject>& Link,
		const TMap<FString, FVector>& NodeLocations,
		FVector& OutLeft,
		FVector& OutRight)
	{
		if (!Link.IsValid())
		{
			return false;
		}
		if ((TryReadVectorField(Link, TEXT("left_cm"), OutLeft) || TryReadVectorField(Link, TEXT("left"), OutLeft))
			&& (TryReadVectorField(Link, TEXT("right_cm"), OutRight) || TryReadVectorField(Link, TEXT("right"), OutRight)))
		{
			return true;
		}

		FString From;
		FString To;
		if (!Link->TryGetStringField(TEXT("from"), From))
		{
			Link->TryGetStringField(TEXT("from_id"), From);
		}
		if (!Link->TryGetStringField(TEXT("to"), To))
		{
			Link->TryGetStringField(TEXT("to_id"), To);
		}
		const FVector* FromLocation = NodeLocations.Find(From);
		const FVector* ToLocation = NodeLocations.Find(To);
		if (FromLocation && ToLocation)
		{
			OutLeft = *FromLocation;
			OutRight = *ToLocation;
			return true;
		}
		return false;
	}

	static bool RunArchitectureNavlinkGenerate(
		const FSololmcpToolExecutionContext& Context,
		const TArray<FString>& RequiredReceipts,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_navlink_generate"));
		Out->SetArrayField(TEXT("required_receipts"), StringArrayJson(RequiredReceipts));
		Out->SetBoolField(TEXT("mutating"), true);

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("navlink_writer_ready_dry_run"));
			Summary = TEXT("architecture_navlink_generate dry-run contract returned; pass execute=true to spawn NavLinkProxy actors.");
			return true;
		}

		TSharedPtr<FJsonObject> Graph;
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
		TryResolveInteriorGraph(Arguments, Graph, Nodes, Links);
		const TArray<TSharedPtr<FJsonValue>>* ExplicitNavLinks = nullptr;
		if (Arguments->TryGetArrayField(TEXT("nav_links"), ExplicitNavLinks) && ExplicitNavLinks)
		{
			Links = ExplicitNavLinks;
		}
		if (!Links || Links->IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("nav_links or interior_nav_graph.links"));
			Error = TEXT("Missing nav_links or interior_nav_graph.links.");
			return false;
		}

		TMap<FString, FVector> NodeLocations;
		TSet<FString> NodeIds;
		int32 EntranceNodeCount = 0;
		BuildInteriorNodeMap(Nodes, NodeLocations, NodeIds, EntranceNodeCount);

		UWorld* World = GetEditorWorld(Context, Out, Error);
		if (!World)
		{
			return false;
		}

		FString AssemblyId;
		Arguments->TryGetStringField(TEXT("assembly_id"), AssemblyId);
		if (AssemblyId.IsEmpty())
		{
			AssemblyId = FString::Printf(TEXT("navlinks_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		}
		const FName AssemblyTag(*FString::Printf(TEXT("SOMOLArchitectureAssembly:%s"), *AssemblyId));

		TArray<TSharedPtr<FJsonValue>> SpawnedLinks;
		TArray<FString> Failures;
		bool bDirtyQueued = false;
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ArchitectureNavlinkGenerate", "SOMOLMCP Architecture NavLink Generate"));
		for (int32 Index = 0; Index < Links->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Link = (*Links)[Index].IsValid() ? (*Links)[Index]->AsObject() : nullptr;
			FVector Left = FVector::ZeroVector;
			FVector Right = FVector::ZeroVector;
			if (!ResolveNavLinkEndpoints(Link, NodeLocations, Left, Right))
			{
				Failures.Add(FString::Printf(TEXT("nav_link[%d]_missing_endpoints"), Index));
				continue;
			}

			FString LinkId;
			if (!Link.IsValid() || (!Link->TryGetStringField(TEXT("id"), LinkId) && !Link->TryGetStringField(TEXT("link_id"), LinkId)) || LinkId.TrimStartAndEnd().IsEmpty())
			{
				LinkId = FString::Printf(TEXT("navlink_%03d"), Index);
			}

			const FVector Midpoint = (Left + Right) * 0.5;
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParams.bAllowDuringConstructionScript = true;
			ANavLinkProxy* Proxy = World->SpawnActor<ANavLinkProxy>(ANavLinkProxy::StaticClass(), FTransform(FRotator::ZeroRotator, Midpoint), SpawnParams);
			if (!Proxy)
			{
				Failures.Add(FString::Printf(TEXT("nav_link[%d]_spawn_failed"), Index));
				continue;
			}
			Proxy->Modify();
			Proxy->SetActorLabel(MakeUniqueActorLabel(World, FString::Printf(TEXT("SOMOL_%s_%s"), *AssemblyId, *LinkId)));
			Proxy->Tags.AddUnique(FName(TEXT("SOMOLArchitectureNavLink")));
			Proxy->Tags.AddUnique(AssemblyTag);
			Proxy->PointLinks.Reset();
			FNavigationLink NavLink(Left - Midpoint, Right - Midpoint);
			FString Direction;
			if (Link.IsValid())
			{
				Link->TryGetStringField(TEXT("direction"), Direction);
			}
			if (Direction.Equals(TEXT("left_to_right"), ESearchCase::IgnoreCase) || Direction.Equals(TEXT("one_way"), ESearchCase::IgnoreCase))
			{
				NavLink.Direction = ENavLinkDirection::LeftToRight;
			}
			else if (Direction.Equals(TEXT("right_to_left"), ESearchCase::IgnoreCase))
			{
				NavLink.Direction = ENavLinkDirection::RightToLeft;
			}
			else
			{
				NavLink.Direction = ENavLinkDirection::BothWays;
			}
			Proxy->PointLinks.Add(NavLink);
			Proxy->SetSmartLinkEnabled(false);
			DirtyNavigationForActor(World, Proxy, bDirtyQueued);
			Proxy->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(Proxy);
			SpawnedLinks.Add(MakeShared<FJsonValueObject>(NavLinkReceiptJson(LinkId, Left, Right, Proxy)));
		}

		bool bRebuildNavigation = false;
		Arguments->TryGetBoolField(TEXT("rebuild_navigation"), bRebuildNavigation);
		bool bRebuildTriggered = false;
		if (bRebuildNavigation)
		{
			if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
			{
				NavSys->Build();
				bRebuildTriggered = true;
			}
		}

		const bool bOk = Failures.IsEmpty() && SpawnedLinks.Num() > 0;
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("navlink_generate_ok") : TEXT("navlink_generate_failed"));
		Out->SetStringField(TEXT("assembly_id"), AssemblyId);
		Out->SetNumberField(TEXT("requested_link_count"), Links->Num());
		Out->SetNumberField(TEXT("spawned_navlink_proxy_count"), SpawnedLinks.Num());
		Out->SetBoolField(TEXT("navigation_dirty_area_queued"), bDirtyQueued);
		Out->SetBoolField(TEXT("navigation_rebuild_triggered"), bRebuildTriggered);
		Out->SetArrayField(TEXT("spawned_navlinks"), SpawnedLinks);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%d:%d:%d"), *AssemblyId, Links->Num(), SpawnedLinks.Num(), Failures.Num())));
		Summary = FString::Printf(TEXT("architecture_navlink_generate spawned %d/%d NavLinkProxy actors."), SpawnedLinks.Num(), Links->Num());
		if (!bOk)
		{
			Error = TEXT("Architecture navlink generation failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static void CollectAssetPathsFromArgs(const TSharedRef<FJsonObject>& Arguments, TArray<FString>& OutAssetPaths)
	{
		OutAssetPaths.Reset();
		FString AssetPath;
		if (Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.TrimStartAndEnd().IsEmpty())
		{
			OutAssetPaths.AddUnique(AssetPath.TrimStartAndEnd());
		}
		const TArray<TSharedPtr<FJsonValue>>* AssetPathValues = nullptr;
		if (Arguments->TryGetArrayField(TEXT("asset_paths"), AssetPathValues) && AssetPathValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *AssetPathValues)
			{
				if (Value.IsValid())
				{
					const FString Path = Value->AsString().TrimStartAndEnd();
					if (!Path.IsEmpty())
					{
						OutAssetPaths.AddUnique(Path);
					}
				}
			}
		}
	}

	static bool AddSimpleCollisionShape(UStaticMesh* StaticMesh, UBodySetup* BodySetup, const FString& CollisionType, FString& OutError)
	{
		if (!StaticMesh || !BodySetup)
		{
			OutError = TEXT("Missing StaticMesh or BodySetup.");
			return false;
		}

		const FBoxSphereBounds Bounds = StaticMesh->GetBounds();
		const FVector Extent = Bounds.BoxExtent;
		const FVector Origin = Bounds.Origin;
		if (Extent.X <= KINDA_SMALL_NUMBER || Extent.Y <= KINDA_SMALL_NUMBER || Extent.Z <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("StaticMesh bounds are too small to generate architecture collision.");
			return false;
		}

		if (CollisionType == TEXT("box"))
		{
			FKBoxElem Box;
			Box.Center = Origin;
			Box.X = Extent.X * 2.0f;
			Box.Y = Extent.Y * 2.0f;
			Box.Z = Extent.Z * 2.0f;
			BodySetup->AggGeom.BoxElems.Add(Box);
			return true;
		}
		if (CollisionType == TEXT("sphere"))
		{
			FKSphereElem Sphere;
			Sphere.Center = Origin;
			Sphere.Radius = Bounds.SphereRadius;
			BodySetup->AggGeom.SphereElems.Add(Sphere);
			return true;
		}
		if (CollisionType == TEXT("capsule"))
		{
			FKSphylElem Capsule;
			Capsule.Center = Origin;
			Capsule.Radius = FMath::Max(Extent.X, Extent.Y);
			Capsule.Length = FMath::Max(0.0f, (Extent.Z * 2.0f) - 2.0f * Capsule.Radius);
			BodySetup->AggGeom.SphylElems.Add(Capsule);
			return true;
		}
		if (CollisionType == TEXT("convex") || CollisionType == TEXT("obb"))
		{
			FKConvexElem Convex;
			const FVector Points[8] = {
				Origin + FVector(-Extent.X, -Extent.Y, -Extent.Z),
				Origin + FVector( Extent.X, -Extent.Y, -Extent.Z),
				Origin + FVector(-Extent.X,  Extent.Y, -Extent.Z),
				Origin + FVector( Extent.X,  Extent.Y, -Extent.Z),
				Origin + FVector(-Extent.X, -Extent.Y,  Extent.Z),
				Origin + FVector( Extent.X, -Extent.Y,  Extent.Z),
				Origin + FVector(-Extent.X,  Extent.Y,  Extent.Z),
				Origin + FVector( Extent.X,  Extent.Y,  Extent.Z),
			};
			Convex.VertexData.Empty(8);
			for (const FVector& Point : Points)
			{
				Convex.VertexData.Add(Point);
			}
			Convex.UpdateElemBox();
			BodySetup->AggGeom.ConvexElems.Add(Convex);
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported architecture collision_type '%s'."), *CollisionType);
		return false;
	}

	static bool RunArchitectureStaticMeshCollisionRepair(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_staticmesh_collision_repair"));
		Out->SetBoolField(TEXT("mutating"), true);

		TArray<FString> AssetPaths;
		CollectAssetPathsFromArgs(Arguments, AssetPaths);
		FString CollisionType = TEXT("box");
		Arguments->TryGetStringField(TEXT("collision_type"), CollisionType);
		CollisionType = CollisionType.TrimStartAndEnd().ToLower();
		if (CollisionType.IsEmpty())
		{
			CollisionType = TEXT("box");
		}
		bool bSaveAsset = false;
		Arguments->TryGetBoolField(TEXT("save_asset"), bSaveAsset);

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("staticmesh_collision_repair_dry_run"));
			Out->SetStringField(TEXT("collision_type"), CollisionType);
			Out->SetArrayField(TEXT("asset_paths"), StringArrayJson(AssetPaths));
			Out->SetStringField(TEXT("recommended_next_tool"), TEXT("architecture_collision_contract_validate"));
			Summary = TEXT("architecture_staticmesh_collision_repair dry-run contract returned; pass execute=true to write simple collision.");
			return true;
		}
		if (AssetPaths.IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("asset_path or asset_paths"));
			Error = TEXT("Missing asset_path or asset_paths.");
			return false;
		}

		int32 MaxAssets = 64;
		Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssets);
		if (AssetPaths.Num() > FMath::Clamp(MaxAssets, 1, 256))
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error_code"), TEXT("ASSET_LIMIT_EXCEEDED"));
			Error = FString::Printf(TEXT("Asset count %d exceeds max_assets %d."), AssetPaths.Num(), MaxAssets);
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ArchitectureStaticMeshCollisionRepair", "SOMOLMCP Architecture StaticMesh Collision Repair"));
		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<FString> Failures;
		int32 RepairedCount = 0;
		int32 SavedCount = 0;
		for (const FString& AssetPath : AssetPaths)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("asset_path"), AssetPath);

			FString LoadError;
			UObject* Asset = Context.Services.LoadAsset(AssetPath, LoadError);
			UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
			if (!StaticMesh)
			{
				const FString Failure = FString::Printf(TEXT("%s: not a UStaticMesh or failed to load (%s)"), *AssetPath, *LoadError);
				Failures.Add(Failure);
				Row->SetBoolField(TEXT("ok"), false);
				Row->SetStringField(TEXT("failure"), Failure);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
				continue;
			}

			StaticMesh->Modify();
			StaticMesh->CreateBodySetup();
			UBodySetup* BodySetup = StaticMesh->GetBodySetup();
			if (!BodySetup)
			{
				const FString Failure = FString::Printf(TEXT("%s: CreateBodySetup returned null"), *AssetPath);
				Failures.Add(Failure);
				Row->SetBoolField(TEXT("ok"), false);
				Row->SetStringField(TEXT("failure"), Failure);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
				continue;
			}

			BodySetup->Modify();
			const int32 BeforeCount = CountSimpleGeoms(BodySetup);
			BodySetup->RemoveSimpleCollision();
			BodySetup->CollisionTraceFlag = CTF_UseDefault;
			FString ShapeError;
			if (!AddSimpleCollisionShape(StaticMesh, BodySetup, CollisionType, ShapeError))
			{
				Failures.Add(FString::Printf(TEXT("%s: %s"), *AssetPath, *ShapeError));
				Row->SetBoolField(TEXT("ok"), false);
				Row->SetStringField(TEXT("failure"), ShapeError);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
				continue;
			}

			BodySetup->InvalidatePhysicsData();
			BodySetup->CreatePhysicsMeshes();
			StaticMesh->PostEditChange();
			StaticMesh->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(StaticMesh);

			bool bSaved = false;
			FString SaveError;
			if (bSaveAsset)
			{
				bSaved = Context.Services.SaveAsset(StaticMesh->GetPathName(), true, SaveError);
				if (bSaved)
				{
					++SavedCount;
				}
			}

			const int32 AfterCount = CountSimpleGeoms(BodySetup);
			++RepairedCount;
			Row->SetBoolField(TEXT("ok"), AfterCount > 0);
			Row->SetStringField(TEXT("asset_name"), StaticMesh->GetName());
			Row->SetNumberField(TEXT("simple_collision_before"), BeforeCount);
			Row->SetNumberField(TEXT("simple_collision_after"), AfterCount);
			Row->SetStringField(TEXT("collision_type"), CollisionType);
			Row->SetBoolField(TEXT("saved"), bSaved);
			if (!SaveError.IsEmpty())
			{
				Row->SetStringField(TEXT("save_error"), SaveError);
			}
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}

		const bool bOk = Failures.IsEmpty() && RepairedCount == AssetPaths.Num();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("staticmesh_collision_repaired") : TEXT("staticmesh_collision_repair_partial"));
		Out->SetStringField(TEXT("collision_type"), CollisionType);
		Out->SetNumberField(TEXT("asset_count"), AssetPaths.Num());
		Out->SetNumberField(TEXT("repaired_count"), RepairedCount);
		Out->SetNumberField(TEXT("saved_count"), SavedCount);
		Out->SetArrayField(TEXT("assets"), Rows);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%d:%d:%s"), AssetPaths.Num(), RepairedCount, Failures.Num(), *CollisionType)));
		Summary = FString::Printf(TEXT("architecture_staticmesh_collision_repair repaired %d/%d assets."), RepairedCount, AssetPaths.Num());
		if (!bOk)
		{
			Error = TEXT("Architecture StaticMesh collision repair completed with failures.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool TrySetActorHlodLayer(AActor* Actor, UHLODLayer* Layer, FString& OutReadback, FString& OutFailure)
	{
		if (!IsValid(Actor))
		{
			OutFailure = TEXT("Actor is invalid.");
			return false;
		}
		FObjectProperty* HlodProperty = CastField<FObjectProperty>(Actor->GetClass()->FindPropertyByName(TEXT("HLODLayer")));
		if (!HlodProperty)
		{
			OutFailure = TEXT("Actor class does not expose HLODLayer property.");
			return false;
		}
		if (Layer && HlodProperty->PropertyClass && !Layer->IsA(HlodProperty->PropertyClass))
		{
			OutFailure = TEXT("Requested HLODLayer is not compatible with actor HLODLayer property.");
			return false;
		}
		HlodProperty->SetObjectPropertyValue_InContainer(Actor, Layer);
		UObject* Readback = HlodProperty->GetObjectPropertyValue_InContainer(Actor);
		OutReadback = Readback ? Readback->GetPathName() : FString();
		const bool bApplied = Readback == Layer;
		if (!bApplied)
		{
			OutFailure = TEXT("HLODLayer readback did not match requested layer.");
		}
		return bApplied;
	}

	static bool RunArchitectureDeploymentMetadataApply(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_deployment_metadata_apply"));
		Out->SetBoolField(TEXT("mutating"), true);

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("deployment_metadata_apply_dry_run"));
			Out->SetStringField(TEXT("recommended_data_layer_tool"), TEXT("world_create_data_layer_actor_membership_apply"));
			Out->SetStringField(TEXT("recommended_hlod_tool"), TEXT("architecture_hlod_build_dispatch"));
			Out->SetArrayField(TEXT("worldforge_followup_plan"), BuildArchitectureWorldForgeFollowupPlan());
			Summary = TEXT("architecture_deployment_metadata_apply dry-run contract returned; pass execute=true to tag actors and set HLOD layer when supplied.");
			return true;
		}

		UWorld* World = GetEditorWorld(Context, Out, Error);
		if (!World)
		{
			return false;
		}
		TArray<AActor*> Actors;
		FString AssemblyId;
		ResolveArchitectureActors(World, Arguments, Actors, AssemblyId);
		if (Actors.IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("status"), TEXT("deployment_metadata_no_actors"));
			Error = TEXT("No architecture actors matched actor_labels, receipt, or assembly_id.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		FString DeploymentId;
		Arguments->TryGetStringField(TEXT("deployment_id"), DeploymentId);
		if (DeploymentId.IsEmpty())
		{
			DeploymentId = AssemblyId.IsEmpty() ? FString::Printf(TEXT("deployment_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)) : AssemblyId;
		}
		FString DistrictId;
		Arguments->TryGetStringField(TEXT("district_id"), DistrictId);
		FString DataLayerName;
		if (!Arguments->TryGetStringField(TEXT("data_layer_name"), DataLayerName))
		{
			Arguments->TryGetStringField(TEXT("data_layer"), DataLayerName);
		}
		FString RuntimeGrid;
		Arguments->TryGetStringField(TEXT("runtime_grid"), RuntimeGrid);
		FString HlodLayerPath;
		Arguments->TryGetStringField(TEXT("hlod_layer_path"), HlodLayerPath);

		UHLODLayer* HlodLayer = nullptr;
		if (!HlodLayerPath.TrimStartAndEnd().IsEmpty())
		{
			HlodLayer = LoadObject<UHLODLayer>(nullptr, *HlodLayerPath);
			if (!HlodLayer)
			{
				Out->SetBoolField(TEXT("ok"), false);
				SololmcpError::InvalidPath(Out, HlodLayerPath);
				Error = FString::Printf(TEXT("HLOD layer not found: %s"), *HlodLayerPath);
				Out->SetStringField(TEXT("error"), Error);
				return false;
			}
		}

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ArchitectureDeploymentMetadataApply", "SOMOLMCP Architecture Deployment Metadata Apply"));
		TArray<TSharedPtr<FJsonValue>> ActorRows;
		TArray<FString> Failures;
		int32 HlodAppliedCount = 0;
		for (AActor* Actor : Actors)
		{
			if (!IsValid(Actor))
			{
				continue;
			}
			Actor->Modify();
			Actor->Tags.AddUnique(FName(*FString::Printf(TEXT("SOMOLArchitectureDeployment:%s"), *DeploymentId)));
			if (!DistrictId.IsEmpty())
			{
				Actor->Tags.AddUnique(FName(*FString::Printf(TEXT("SOMOLSettlementDistrict:%s"), *DistrictId)));
			}
			if (!DataLayerName.IsEmpty())
			{
				Actor->Tags.AddUnique(FName(*FString::Printf(TEXT("SOMOLDataLayerIntent:%s"), *DataLayerName)));
			}
			if (!RuntimeGrid.IsEmpty())
			{
				Actor->Tags.AddUnique(FName(*FString::Printf(TEXT("SOMOLRuntimeGrid:%s"), *RuntimeGrid)));
			}

			FString HlodReadback;
			FString HlodFailure;
			bool bHlodApplied = false;
			if (HlodLayerPath.IsEmpty())
			{
				bHlodApplied = true;
			}
			else
			{
				bHlodApplied = TrySetActorHlodLayer(Actor, HlodLayer, HlodReadback, HlodFailure);
				if (bHlodApplied)
				{
					++HlodAppliedCount;
				}
				else
				{
					Failures.Add(FString::Printf(TEXT("%s: %s"), *Actor->GetActorLabel(), *HlodFailure));
				}
			}

			Actor->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(Actor);
			TSharedRef<FJsonObject> Row = ActorReadbackJson(Actor);
			Row->SetBoolField(TEXT("hlod_layer_applied"), bHlodApplied);
			Row->SetStringField(TEXT("hlod_layer_readback"), HlodReadback);
			if (!HlodFailure.IsEmpty())
			{
				Row->SetStringField(TEXT("hlod_failure"), HlodFailure);
			}
			ActorRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		const bool bOk = Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("deployment_metadata_applied") : TEXT("deployment_metadata_partial"));
		Out->SetStringField(TEXT("assembly_id"), AssemblyId);
		Out->SetStringField(TEXT("deployment_id"), DeploymentId);
		Out->SetStringField(TEXT("district_id"), DistrictId);
		Out->SetStringField(TEXT("data_layer_name"), DataLayerName);
		Out->SetStringField(TEXT("data_layer_membership_followup_tool"), TEXT("world_create_data_layer_actor_membership_apply"));
		Out->SetStringField(TEXT("runtime_grid"), RuntimeGrid);
		Out->SetStringField(TEXT("hlod_layer_path"), HlodLayerPath);
		Out->SetNumberField(TEXT("actor_count"), Actors.Num());
		Out->SetNumberField(TEXT("hlod_layer_applied_count"), HlodAppliedCount);
		Out->SetArrayField(TEXT("actors"), ActorRows);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetArrayField(TEXT("worldforge_followup_plan"), BuildArchitectureWorldForgeFollowupPlan());
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%s:%d:%d:%d"), *AssemblyId, *DeploymentId, Actors.Num(), HlodAppliedCount, Failures.Num())));
		Summary = FString::Printf(TEXT("architecture_deployment_metadata_apply tagged %d actors for %s."), Actors.Num(), *DeploymentId);
		if (!bOk)
		{
			Error = TEXT("Architecture deployment metadata apply completed with failures.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static int32 CountHlodActors(UWorld* World)
	{
		int32 Count = 0;
		if (!World)
		{
			return Count;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && Actor->GetClass() && Actor->GetClass()->GetName().Contains(TEXT("HLOD")))
			{
				++Count;
			}
		}
		return Count;
	}

	static bool RunArchitectureHlodBuildDispatch(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_hlod_build_dispatch"));
		Out->SetBoolField(TEXT("mutating"), true);

		FString Mode = TEXT("changed");
		Arguments->TryGetStringField(TEXT("mode"), Mode);
		Mode = Mode.TrimStartAndEnd().ToLower();
		int32 HlodLayerIndex = 0;
		Arguments->TryGetNumberField(TEXT("hlod_layer_index"), HlodLayerIndex);

		FString Command;
		if (Mode == TEXT("all") || Mode == TEXT("generate"))
		{
			Command = FString::Printf(TEXT("wp.HLOD.Generate %d"), FMath::Max(0, HlodLayerIndex));
		}
		else if (Mode == TEXT("rebuild"))
		{
			Command = TEXT("wp.HLOD.RebuildHLODs");
		}
		else
		{
			Mode = TEXT("changed");
			Command = TEXT("wp.HLOD.BuildChanged");
		}
		FString JobId;
		ReadStringFieldAny(Arguments, { TEXT("job_id"), TEXT("hlod_job_id") }, JobId);
		if (JobId.IsEmpty())
		{
			JobId = FString::Printf(TEXT("architecture_hlod_%s"), *MakeReceiptHash(FString::Printf(TEXT("%s:%s:%d"), *Command, *Mode, HlodLayerIndex)));
		}
		TSharedRef<FJsonObject> PollArgs = MakeShared<FJsonObject>();
		PollArgs->SetStringField(TEXT("job_id"), JobId);
		PollArgs->SetStringField(TEXT("dispatch_receipt"), TEXT("$architecture_hlod_build_dispatch"));
		PollArgs->SetStringField(TEXT("target_receipt"), TEXT("$architecture_hlod_build_dispatch"));

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("hlod_build_dispatch_dry_run"));
			Out->SetStringField(TEXT("job_id"), JobId);
			Out->SetStringField(TEXT("mode"), Mode);
			Out->SetStringField(TEXT("dispatch"), Command);
			Out->SetStringField(TEXT("completion_poll_tool"), TEXT("world_create_hlod_job_poll"));
			Out->SetObjectField(TEXT("poll_args"), PollArgs);
			Summary = FString::Printf(TEXT("architecture_hlod_build_dispatch dry-run planned %s."), *Command);
			return true;
		}

		UWorld* World = GetEditorWorld(Context, Out, Error);
		if (!World)
		{
			return false;
		}
		const int32 HlodActorsBefore = CountHlodActors(World);
		TSharedRef<FJsonObject> ConsoleOut = MakeShared<FJsonObject>();
		FString ConsoleSummary;
		FString ConsoleError;
		const bool bExecOk = Context.Services.ExecuteConsole(Command, ConsoleOut, ConsoleSummary, ConsoleError);
		const int32 HlodActorsAfter = CountHlodActors(World);
		Out->SetBoolField(TEXT("ok"), bExecOk && ConsoleError.IsEmpty());
		Out->SetStringField(TEXT("status"), bExecOk && ConsoleError.IsEmpty() ? TEXT("hlod_build_dispatched_async") : TEXT("hlod_build_dispatch_failed"));
		Out->SetStringField(TEXT("job_id"), JobId);
		Out->SetStringField(TEXT("mode"), Mode);
		Out->SetStringField(TEXT("dispatch"), Command);
		Out->SetBoolField(TEXT("async"), true);
		Out->SetBoolField(TEXT("completed"), false);
		Out->SetStringField(TEXT("completion_poll_tool"), TEXT("world_create_hlod_job_poll"));
		Out->SetObjectField(TEXT("poll_args"), PollArgs);
		Out->SetNumberField(TEXT("hlod_actor_count_before"), HlodActorsBefore);
		Out->SetNumberField(TEXT("hlod_actor_count_after_dispatch"), HlodActorsAfter);
		Out->SetObjectField(TEXT("console"), ConsoleOut);
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%d:%d:%d"), *Command, bExecOk ? 1 : 0, HlodActorsBefore, HlodActorsAfter)));
		Summary = FString::Printf(TEXT("architecture_hlod_build_dispatch dispatched %s."), *Command);
		if (!bExecOk || !ConsoleError.IsEmpty())
		{
			Error = ConsoleError.IsEmpty() ? TEXT("HLOD build dispatch failed.") : ConsoleError;
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static double PathLengthCm(const TArray<FVector>& Points)
	{
		double Length = 0.0;
		for (int32 Index = 1; Index < Points.Num(); ++Index)
		{
			Length += FVector::Distance(Points[Index - 1], Points[Index]);
		}
		return Length;
	}

	static bool RunArchitectureNavmeshPathSample(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_navmesh_path_sample"));

		UWorld* World = GetEditorWorld(Context, Out, Error);
		if (!World)
		{
			return false;
		}
		UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		if (!NavSys)
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("status"), TEXT("navigation_system_unavailable"));
			Error = TEXT("NavigationSystemV1 is unavailable in the current world.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		TArray<TPair<FVector, FVector>> Samples;
		const TArray<TSharedPtr<FJsonValue>>* SampleValues = nullptr;
		if (Arguments->TryGetArrayField(TEXT("path_samples"), SampleValues) && SampleValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *SampleValues)
			{
				const TSharedPtr<FJsonObject> Sample = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Sample.IsValid())
				{
					continue;
				}
				FVector Start = FVector::ZeroVector;
				FVector End = FVector::ZeroVector;
				if ((TryReadVectorField(Sample, TEXT("start_cm"), Start) || TryReadVectorField(Sample, TEXT("start"), Start))
					&& (TryReadVectorField(Sample, TEXT("end_cm"), End) || TryReadVectorField(Sample, TEXT("end"), End)))
				{
					Samples.Add(TPair<FVector, FVector>(Start, End));
				}
			}
		}

		if (Samples.IsEmpty())
		{
			TSharedPtr<FJsonObject> Graph;
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
			if (TryResolveInteriorGraph(Arguments, Graph, Nodes, Links) && Links)
			{
				TMap<FString, FVector> NodeLocations;
				TSet<FString> NodeIds;
				int32 EntranceNodeCount = 0;
				BuildInteriorNodeMap(Nodes, NodeLocations, NodeIds, EntranceNodeCount);
				for (const TSharedPtr<FJsonValue>& LinkValue : *Links)
				{
					const TSharedPtr<FJsonObject> Link = LinkValue.IsValid() ? LinkValue->AsObject() : nullptr;
					FVector Left = FVector::ZeroVector;
					FVector Right = FVector::ZeroVector;
					if (ResolveNavLinkEndpoints(Link, NodeLocations, Left, Right))
					{
						Samples.Add(TPair<FVector, FVector>(Left, Right));
					}
				}
			}
		}

		int32 MaxSamples = 64;
		Arguments->TryGetNumberField(TEXT("max_samples"), MaxSamples);
		if (Samples.Num() > FMath::Clamp(MaxSamples, 1, 512))
		{
			Samples.SetNum(FMath::Clamp(MaxSamples, 1, 512));
		}
		if (Samples.IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("path_samples or interior_nav_graph.links with node locations"));
			Error = TEXT("Missing path_samples or interior_nav_graph.links with node locations.");
			return false;
		}

		bool bRequireCompletePaths = true;
		Arguments->TryGetBoolField(TEXT("require_complete_paths"), bRequireCompletePaths);
		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<FString> Failures;
		int32 FoundCount = 0;
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			const FVector Start = Samples[Index].Key;
			const FVector End = Samples[Index].Value;
			UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, Start, End);
			const bool bPathValid = Path && Path->IsValid() && Path->PathPoints.Num() > 0;
			const bool bPartial = Path ? Path->IsPartial() : true;
			const bool bFound = bPathValid && (!bRequireCompletePaths || !bPartial);
			if (bFound)
			{
				++FoundCount;
			}
			else
			{
				Failures.Add(FString::Printf(TEXT("path_sample[%d]_not_reachable"), Index));
			}

			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), Index);
			Row->SetObjectField(TEXT("start_cm"), VectorJson(Start));
			Row->SetObjectField(TEXT("end_cm"), VectorJson(End));
			Row->SetBoolField(TEXT("path_found"), bFound);
			Row->SetBoolField(TEXT("path_valid"), bPathValid);
			Row->SetBoolField(TEXT("partial"), bPartial);
			Row->SetNumberField(TEXT("path_point_count"), Path ? Path->PathPoints.Num() : 0);
			Row->SetNumberField(TEXT("path_length_cm"), Path ? PathLengthCm(Path->PathPoints) : -1.0);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}

		const bool bOk = Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("navmesh_path_samples_ok") : TEXT("navmesh_path_samples_failed"));
		Out->SetNumberField(TEXT("sample_count"), Samples.Num());
		Out->SetNumberField(TEXT("path_found_count"), FoundCount);
		Out->SetBoolField(TEXT("require_complete_paths"), bRequireCompletePaths);
		Out->SetArrayField(TEXT("path_samples"), Rows);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%d:%d"), Samples.Num(), FoundCount, Failures.Num())));
		Summary = FString::Printf(TEXT("architecture_navmesh_path_sample found %d/%d paths."), FoundCount, Samples.Num());
		if (!bOk)
		{
			Error = TEXT("Architecture navmesh path sampling failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool TextContainsAnyToken(const FString& Text, const TArray<FString>& Tokens)
	{
		for (const FString& Token : Tokens)
		{
			if (!Token.IsEmpty() && Text.Contains(Token, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static bool JsonValueContainsAnyToken(const TSharedPtr<FJsonValue>& Value, const TArray<FString>& Tokens);

	static bool JsonObjectContainsAnyToken(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Tokens)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (TextContainsAnyToken(Pair.Key, Tokens) || JsonValueContainsAnyToken(Pair.Value, Tokens))
			{
				return true;
			}
		}
		return false;
	}

	static bool JsonValueContainsAnyToken(const TSharedPtr<FJsonValue>& Value, const TArray<FString>& Tokens)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return false;
		}
		if (Value->Type == EJson::String)
		{
			return TextContainsAnyToken(Value->AsString(), Tokens);
		}
		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Child : Value->AsArray())
			{
				if (JsonValueContainsAnyToken(Child, Tokens))
				{
					return true;
				}
			}
			return false;
		}
		if (Value->Type == EJson::Object)
		{
			return JsonObjectContainsAnyToken(Value->AsObject(), Tokens);
		}
		return false;
	}

	static bool JsonObjectHasPositiveReceiptStatus(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		bool BoolValue = false;
		if (Object->TryGetBoolField(TEXT("ok"), BoolValue))
		{
			return BoolValue;
		}
		if (Object->TryGetBoolField(TEXT("success"), BoolValue))
		{
			return BoolValue;
		}
		if (Object->TryGetBoolField(TEXT("passed"), BoolValue))
		{
			return BoolValue;
		}
		FString Status;
		if (Object->TryGetStringField(TEXT("status"), Status))
		{
			Status = Status.ToLower();
			if (Status.Contains(TEXT("failed"))
				|| Status.Contains(TEXT("failure"))
				|| Status.Contains(TEXT("invalid"))
				|| Status.Contains(TEXT("blocked"))
				|| Status.Contains(TEXT("missing"))
				|| Status.Contains(TEXT("error")))
			{
				return false;
			}
			return Status.Contains(TEXT("ok"))
				|| Status.Contains(TEXT("valid"))
				|| Status.Contains(TEXT("passed"))
				|| Status.Contains(TEXT("ready"))
				|| Status.Contains(TEXT("applied"))
				|| Status.Contains(TEXT("dispatch"))
				|| Status.Contains(TEXT("saved"))
				|| Status.Contains(TEXT("reload"))
				|| Status.Contains(TEXT("screenshot"))
				|| Status.Contains(TEXT("samples"));
		}
		return false;
	}

	static bool JsonValuePassesStrictReceiptCheck(const TSharedPtr<FJsonValue>& Value, const TArray<FString>& Tokens)
	{
		if (!JsonValueContainsAnyToken(Value, Tokens))
		{
			return false;
		}
		if (!Value.IsValid() || Value->IsNull())
		{
			return false;
		}
		if (Value->Type == EJson::Object)
		{
			return JsonObjectHasPositiveReceiptStatus(Value->AsObject());
		}
		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Child : Value->AsArray())
			{
				if (JsonValuePassesStrictReceiptCheck(Child, Tokens))
				{
					return true;
				}
			}
			return false;
		}
		return false;
	}

	static TArray<FString> GateTokens(const FString& Gate)
	{
		const FString Normalized = Gate.ToLower();
		if (Normalized == TEXT("assembly"))
		{
			return { TEXT("assembly_executed"), TEXT("assembly_readback"), TEXT("spawned_actors"), TEXT("assembly_id") };
		}
		if (Normalized == TEXT("collision"))
		{
			return { TEXT("collision_audit_ok"), TEXT("collision_components_enabled"), TEXT("simple_collision"), TEXT("collision_enabled") };
		}
		if (Normalized == TEXT("reachability"))
		{
			return { TEXT("reachability_contract_ok"), TEXT("connected_node_count"), TEXT("entrance_node_count") };
		}
		if (Normalized == TEXT("nav") || Normalized == TEXT("navmesh"))
		{
			return { TEXT("navmesh_path_samples_ok"), TEXT("navlink_generate_ok"), TEXT("interior_nav_build_ready"), TEXT("navigation_rebuild_triggered") };
		}
		if (Normalized == TEXT("data_layer"))
		{
			return { TEXT("data_layer"), TEXT("DataLayer"), TEXT("data_layer_membership"), TEXT("data_layer_state") };
		}
		if (Normalized == TEXT("hlod"))
		{
			return { TEXT("hlod"), TEXT("HLOD"), TEXT("hlod_build_dispatched"), TEXT("HLODLayer") };
		}
		if (Normalized == TEXT("save"))
		{
			return { TEXT("packages_saved"), TEXT("saved_packages"), TEXT("save_receipt"), TEXT("saved") };
		}
		if (Normalized == TEXT("reload"))
		{
			return { TEXT("reload"), TEXT("reload_verify"), TEXT("reloaded") };
		}
		if (Normalized == TEXT("screenshot") || Normalized == TEXT("visual"))
		{
			return { TEXT("screenshot"), TEXT("viewport"), TEXT("pixel"), TEXT("image") };
		}
		return { Gate };
	}

	static bool RunArchitectureProductionGateValidate(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_production_gate_validate"));

		TArray<FString> RequiredGates = { TEXT("assembly"), TEXT("collision"), TEXT("reachability") };
		bool bRequireWorldForgeChain = false;
		Arguments->TryGetBoolField(TEXT("require_worldforge_chain"), bRequireWorldForgeChain);
		bool bStrictReceipts = false;
		Arguments->TryGetBoolField(TEXT("strict_receipts"), bStrictReceipts);
		if (bRequireWorldForgeChain)
		{
			RequiredGates.AddUnique(TEXT("nav"));
			RequiredGates.AddUnique(TEXT("data_layer"));
			RequiredGates.AddUnique(TEXT("hlod"));
			RequiredGates.AddUnique(TEXT("save"));
			RequiredGates.AddUnique(TEXT("reload"));
			RequiredGates.AddUnique(TEXT("screenshot"));
		}
		TryGetStringArrayField(Arguments, TEXT("required_gates"), RequiredGates);

		const TArray<TSharedPtr<FJsonValue>>* Receipts = nullptr;
		if (!Arguments->TryGetArrayField(TEXT("prior_receipts"), Receipts) || !Receipts)
		{
			Arguments->TryGetArrayField(TEXT("receipts"), Receipts);
		}
		TArray<TSharedPtr<FJsonValue>> SingleReceiptValues;
		if (!Receipts)
		{
			if (const TSharedPtr<FJsonObject>* ReceiptObj = nullptr; Arguments->TryGetObjectField(TEXT("receipt"), ReceiptObj) && ReceiptObj && ReceiptObj->IsValid())
			{
				SingleReceiptValues.Add(MakeShared<FJsonValueObject>(*ReceiptObj));
				Receipts = &SingleReceiptValues;
			}
		}
		if (!Receipts)
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("prior_receipts"));
			Error = TEXT("Missing prior_receipts, receipts, or receipt.");
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> GateRows;
		TArray<FString> MissingGates;
		for (const FString& Gate : RequiredGates)
		{
			const TArray<FString> Tokens = GateTokens(Gate);
			bool bFound = false;
			for (const TSharedPtr<FJsonValue>& ReceiptValue : *Receipts)
			{
				if (bStrictReceipts
					? JsonValuePassesStrictReceiptCheck(ReceiptValue, Tokens)
					: JsonValueContainsAnyToken(ReceiptValue, Tokens))
				{
					bFound = true;
					break;
				}
			}
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("gate"), Gate);
			Row->SetBoolField(TEXT("present"), bFound);
			Row->SetBoolField(TEXT("strict_receipts"), bStrictReceipts);
			Row->SetArrayField(TEXT("tokens"), StringArrayJson(Tokens));
			GateRows.Add(MakeShared<FJsonValueObject>(Row));
			if (!bFound)
			{
				MissingGates.Add(Gate);
			}
		}

		const bool bOk = MissingGates.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("architecture_production_gate_ok") : TEXT("architecture_production_gate_failed"));
		Out->SetBoolField(TEXT("require_worldforge_chain"), bRequireWorldForgeChain);
		Out->SetBoolField(TEXT("strict_receipts"), bStrictReceipts);
		Out->SetNumberField(TEXT("receipt_count"), Receipts->Num());
		Out->SetArrayField(TEXT("required_gates"), StringArrayJson(RequiredGates));
		Out->SetArrayField(TEXT("gate_results"), GateRows);
		Out->SetArrayField(TEXT("missing_gates"), StringArrayJson(MissingGates));
		Out->SetArrayField(TEXT("missing_gate_followup_plan"), BuildArchitectureMissingGateFollowupPlan(MissingGates));
		Out->SetArrayField(TEXT("worldforge_followup_plan"), BuildArchitectureWorldForgeFollowupPlan());
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%d:%d"), Receipts->Num(), RequiredGates.Num(), MissingGates.Num())));
		Summary = FString::Printf(TEXT("architecture_production_gate_validate passed %d/%d gates."), RequiredGates.Num() - MissingGates.Num(), RequiredGates.Num());
		if (!bOk)
		{
			Error = TEXT("Architecture production gate is missing required receipts.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunArchitectureCityScaleGateValidate(
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("architecture_city_scale_gate_validate"));

		TSharedPtr<FJsonObject> BatchPlan;
		if (const TSharedPtr<FJsonObject>* CityBatchPlanPtr = nullptr; Arguments->TryGetObjectField(TEXT("city_batch_plan"), CityBatchPlanPtr) && CityBatchPlanPtr && CityBatchPlanPtr->IsValid())
		{
			BatchPlan = *CityBatchPlanPtr;
		}
		else if (const TSharedPtr<FJsonObject>* AliasBatchPlanPtr = nullptr; Arguments->TryGetObjectField(TEXT("batch_plan"), AliasBatchPlanPtr) && AliasBatchPlanPtr && AliasBatchPlanPtr->IsValid())
		{
			BatchPlan = *AliasBatchPlanPtr;
		}
		else if (const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr; Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) && ReceiptPtr && ReceiptPtr->IsValid())
		{
			if (const TSharedPtr<FJsonObject>* ReceiptPlanPtr = nullptr; (*ReceiptPtr)->TryGetObjectField(TEXT("city_batch_plan"), ReceiptPlanPtr) && ReceiptPlanPtr && ReceiptPlanPtr->IsValid())
			{
				BatchPlan = *ReceiptPlanPtr;
			}
		}
		if (!BatchPlan.IsValid())
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("city_batch_plan"));
			Error = TEXT("Missing city_batch_plan, batch_plan, or receipt.city_batch_plan.");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Batches = nullptr;
		if (!BatchPlan->TryGetArrayField(TEXT("batches"), Batches) || !Batches || Batches->IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			SololmcpError::MissingParam(Out, TEXT("city_batch_plan.batches"));
			Error = TEXT("Missing city_batch_plan.batches.");
			return false;
		}

		double MinBuildingValue = 1.0;
		ReadNumberFieldAny(Arguments, { TEXT("acceptance_target_building_count"), TEXT("min_building_count"), TEXT("required_building_count") }, MinBuildingValue);
		const int32 MinBuildingCount = FMath::Max(1, FMath::RoundToInt(MinBuildingValue));

		double MaxBuildingsPerBatchValue = 256.0;
		ReadNumberFieldAny(Arguments, { TEXT("max_buildings_per_batch") }, MaxBuildingsPerBatchValue);
		const int32 MaxBuildingsPerBatch = FMath::Clamp(FMath::RoundToInt(MaxBuildingsPerBatchValue), 1, 512);

		double MaxModulesPerBatchValue = 10000.0;
		ReadNumberFieldAny(Arguments, { TEXT("max_modules_per_batch") }, MaxModulesPerBatchValue);
		const int32 MaxModulesPerBatch = FMath::Clamp(FMath::RoundToInt(MaxModulesPerBatchValue), 32, 20000);

		bool bRequireReceipts = true;
		Arguments->TryGetBoolField(TEXT("require_receipts"), bRequireReceipts);
		bool bStrictReceipts = false;
		Arguments->TryGetBoolField(TEXT("strict_receipts"), bStrictReceipts);

		TArray<FString> RequiredGlobalGates = {
			TEXT("nav"),
			TEXT("data_layer"),
			TEXT("hlod"),
			TEXT("save"),
			TEXT("reload"),
			TEXT("screenshot")
		};
		TryGetStringArrayField(Arguments, TEXT("required_global_gates"), RequiredGlobalGates);

		const TArray<TSharedPtr<FJsonValue>>* Receipts = nullptr;
		if (!Arguments->TryGetArrayField(TEXT("prior_receipts"), Receipts) || !Receipts)
		{
			Arguments->TryGetArrayField(TEXT("receipts"), Receipts);
		}

		TArray<FString> Failures;
		TArray<FString> BatchIds;
		TArray<FString> MissingBatchReceipts;
		TArray<TSharedPtr<FJsonValue>> BatchRows;
		int32 PlannedBuildingCount = 0;
		int32 PlannedModuleCount = 0;
		int32 MaxObservedBuildings = 0;
		int32 MaxObservedModules = 0;

		for (int32 Index = 0; Index < Batches->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Batch = (*Batches)[Index].IsValid() ? (*Batches)[Index]->AsObject() : nullptr;
			FString BatchId;
			ReadStringFieldAny(Batch, { TEXT("batch_id"), TEXT("id") }, BatchId);
			BatchId = MakeArchitectureIdToken(BatchId, FString::Printf(TEXT("batch_%03d"), Index + 1));
			double BatchBuildingsValue = 0.0;
			ReadNumberFieldAny(Batch, { TEXT("building_count") }, BatchBuildingsValue);
			double BatchModulesValue = 0.0;
			ReadNumberFieldAny(Batch, { TEXT("estimated_module_count"), TEXT("module_count") }, BatchModulesValue);
			const int32 BatchBuildings = FMath::RoundToInt(BatchBuildingsValue);
			const int32 BatchModules = FMath::RoundToInt(BatchModulesValue);

			BatchIds.Add(BatchId);
			PlannedBuildingCount += BatchBuildings;
			PlannedModuleCount += BatchModules;
			MaxObservedBuildings = FMath::Max(MaxObservedBuildings, BatchBuildings);
			MaxObservedModules = FMath::Max(MaxObservedModules, BatchModules);

			TArray<FString> BatchFailures;
			if (BatchBuildings <= 0)
			{
				BatchFailures.Add(TEXT("empty_batch"));
			}
			if (BatchBuildings > MaxBuildingsPerBatch)
			{
				BatchFailures.Add(TEXT("batch_building_count_exceeds_guard"));
			}
			if (BatchModules > MaxModulesPerBatch)
			{
				BatchFailures.Add(TEXT("batch_module_count_exceeds_guard"));
			}
			if (bRequireReceipts)
			{
				bool bBatchReceiptFound = false;
				if (Receipts)
				{
					for (const TSharedPtr<FJsonValue>& ReceiptValue : *Receipts)
					{
						if (bStrictReceipts
							? JsonValuePassesStrictReceiptCheck(ReceiptValue, { BatchId })
							: JsonValueContainsAnyToken(ReceiptValue, { BatchId }))
						{
							bBatchReceiptFound = true;
							break;
						}
					}
				}
				if (!bBatchReceiptFound)
				{
					MissingBatchReceipts.Add(BatchId);
					BatchFailures.Add(TEXT("missing_batch_receipt"));
				}
			}

			for (const FString& Failure : BatchFailures)
			{
				Failures.Add(FString::Printf(TEXT("%s:%s"), *BatchId, *Failure));
			}

			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("batch_id"), BatchId);
			Row->SetNumberField(TEXT("building_count"), BatchBuildings);
			Row->SetNumberField(TEXT("estimated_module_count"), BatchModules);
			Row->SetArrayField(TEXT("failures"), StringArrayJson(BatchFailures));
			BatchRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		TArray<TSharedPtr<FJsonValue>> GateRows;
		TArray<FString> MissingGlobalGates;
		if (bRequireReceipts && (!Receipts || Receipts->IsEmpty()))
		{
			Failures.Add(TEXT("missing_prior_receipts"));
		}
		else if (bRequireReceipts)
		{
			for (const FString& Gate : RequiredGlobalGates)
			{
				const TArray<FString> Tokens = GateTokens(Gate);
				bool bFound = false;
				for (const TSharedPtr<FJsonValue>& ReceiptValue : *Receipts)
				{
					if (bStrictReceipts
						? JsonValuePassesStrictReceiptCheck(ReceiptValue, Tokens)
						: JsonValueContainsAnyToken(ReceiptValue, Tokens))
					{
						bFound = true;
						break;
					}
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("gate"), Gate);
				Row->SetBoolField(TEXT("present"), bFound);
				Row->SetBoolField(TEXT("strict_receipts"), bStrictReceipts);
				Row->SetArrayField(TEXT("tokens"), StringArrayJson(Tokens));
				GateRows.Add(MakeShared<FJsonValueObject>(Row));
				if (!bFound)
				{
					MissingGlobalGates.Add(Gate);
					Failures.Add(FString::Printf(TEXT("missing_global_gate:%s"), *Gate));
				}
			}
		}

		if (PlannedBuildingCount < MinBuildingCount)
		{
			Failures.Add(TEXT("planned_building_count_below_required_minimum"));
		}

		const bool bOk = Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("city_scale_gate_ok") : TEXT("city_scale_gate_failed"));
		Out->SetNumberField(TEXT("batch_count"), Batches->Num());
		Out->SetNumberField(TEXT("planned_building_count"), PlannedBuildingCount);
		Out->SetNumberField(TEXT("planned_module_count"), PlannedModuleCount);
		Out->SetNumberField(TEXT("min_building_count"), MinBuildingCount);
		Out->SetNumberField(TEXT("acceptance_target_building_count"), MinBuildingCount);
		Out->SetNumberField(TEXT("max_observed_buildings_per_batch"), MaxObservedBuildings);
		Out->SetNumberField(TEXT("max_observed_modules_per_batch"), MaxObservedModules);
		Out->SetBoolField(TEXT("require_receipts"), bRequireReceipts);
		Out->SetBoolField(TEXT("strict_receipts"), bStrictReceipts);
		Out->SetArrayField(TEXT("batch_ids"), StringArrayJson(BatchIds));
		Out->SetArrayField(TEXT("batch_results"), BatchRows);
		Out->SetArrayField(TEXT("missing_batch_receipts"), StringArrayJson(MissingBatchReceipts));
		Out->SetArrayField(TEXT("required_global_gates"), StringArrayJson(RequiredGlobalGates));
		Out->SetArrayField(TEXT("global_gate_results"), GateRows);
		Out->SetArrayField(TEXT("missing_global_gates"), StringArrayJson(MissingGlobalGates));
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetArrayField(TEXT("missing_gate_followup_plan"), BuildArchitectureMissingGateFollowupPlan(MissingGlobalGates));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%d:%d:%d:%d:%d"), Batches->Num(), PlannedBuildingCount, PlannedModuleCount, MissingBatchReceipts.Num(), MissingGlobalGates.Num())));
		Summary = FString::Printf(TEXT("architecture_city_scale_gate_validate checked %d buildings across %d batches."), PlannedBuildingCount, Batches->Num());
		if (!bOk)
		{
			Error = TEXT("Architecture city scale gate failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		return true;
	}

	static bool RunReceiptGatedArchitectureTool(
		const FSololmcpToolExecutionContext& Context,
		const FString& ToolName,
		const TArray<FString>& RequiredReceipts,
		bool bMutating,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		if (ToolName == TEXT("architecture_assembly_execute"))
		{
			return RunArchitectureAssemblyExecute(Context, RequiredReceipts, Arguments, Out, Summary, Error);
		}
		if (ToolName == TEXT("architecture_assembly_readback"))
		{
			return RunArchitectureAssemblyReadback(Context, Arguments, Out, Summary, Error);
		}
		if (ToolName == TEXT("architecture_collision_generate"))
		{
			return RunArchitectureCollisionGenerate(Context, RequiredReceipts, Arguments, Out, Summary, Error);
		}
		if (ToolName == TEXT("architecture_collision_audit"))
		{
			return RunArchitectureCollisionAudit(Context, Arguments, Out, Summary, Error);
		}
		if (ToolName == TEXT("architecture_reachability_audit"))
		{
			return RunArchitectureReachabilityAudit(Context, Arguments, Out, Summary, Error);
		}
		if (ToolName == TEXT("architecture_interior_nav_build"))
		{
			return RunArchitectureInteriorNavBuild(Context, RequiredReceipts, Arguments, Out, Summary, Error);
		}
		if (ToolName == TEXT("architecture_navlink_generate"))
		{
			return RunArchitectureNavlinkGenerate(Context, RequiredReceipts, Arguments, Out, Summary, Error);
		}
		if (ToolName == TEXT("settlement_road_to_entrance_validate"))
		{
			return RunSettlementRoadToEntranceValidate(Arguments, Out, Summary, Error);
		}
		if (ToolName == TEXT("settlement_layout_plan_build"))
		{
			return RunSettlementLayoutPlanBuild(Arguments, Out, Summary, Error);
		}
		if (ToolName == TEXT("architecture_assembly_plan_build") || ToolName == TEXT("architecture_modular_assembly_plan"))
		{
			return RunArchitectureModularAssemblyPlan(Arguments, Out, Summary, Error, ToolName);
		}
		if (ToolName == TEXT("bridge_passability_audit"))
		{
			return RunBridgePassabilityAudit(Arguments, Out, Summary, Error);
		}
		if (ToolName == TEXT("fortress_gate_nav_validate"))
		{
			return RunFortressGateNavValidate(Arguments, Out, Summary, Error);
		}

		SetArchitectureReceiptBase(Out, ToolName);
		Out->SetStringField(TEXT("status"), bMutating ? TEXT("architecture_writer_contract_ready") : TEXT("architecture_validation_contract_ready"));
		Out->SetArrayField(TEXT("required_receipts"), StringArrayJson(RequiredReceipts));
		Out->SetBoolField(TEXT("mutating"), bMutating);

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		if (bMutating && bExecute)
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("status"), TEXT("blocked_pending_concrete_architecture_writer"));
			Out->SetStringField(TEXT("error_code"), TEXT("BLOCKED_PENDING_CONCRETE_ARCHITECTURE_WRITER"));
			Out->SetStringField(TEXT("failure_route"), TEXT("run_dry_run_then_promote_editor_writer_with_rollback_readback_collision_nav_receipts"));
			Error = FString::Printf(TEXT("%s execute=true is blocked until the concrete editor writer has live rollback/readback proof."), *ToolName);
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}

		Out->SetBoolField(TEXT("ok"), true);
		Summary = FString::Printf(TEXT("%s returned architecture receipt contract."), *ToolName);
		return true;
	}

		static bool LoadJsonObjectFromPath(const FString& Path, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		FString Body;
		if (!FFileHelper::LoadFileToString(Body, *Path))
		{
			OutError = FString::Printf(TEXT("Failed to read JSON file: %s"), *Path);
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to parse JSON file: %s"), *Path);
			return false;
		}
		return true;
	}

	static bool ResolveObjectOrPath(const TSharedRef<FJsonObject>& Arguments, const TCHAR* ObjectField, const TCHAR* PathField, TSharedPtr<FJsonObject>& OutObject, FString& OutPath, FString& OutError)
	{
		if (const TSharedPtr<FJsonObject>* Obj = nullptr; Arguments->TryGetObjectField(ObjectField, Obj) && Obj && Obj->IsValid())
		{
			OutObject = *Obj;
			return true;
		}
		if (Arguments->TryGetStringField(PathField, OutPath) && !OutPath.TrimStartAndEnd().IsEmpty())
		{
			return LoadJsonObjectFromPath(OutPath, OutObject, OutError);
		}
		OutError = FString::Printf(TEXT("Missing %s or %s."), ObjectField, PathField);
		return false;
	}

	static bool JsonHasNonEmptyString(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		FString Value;
		return Object.IsValid() && Object->TryGetStringField(FieldName, Value) && !Value.TrimStartAndEnd().IsEmpty();
	}

	static bool JsonStringStartsWith(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const FString& Prefix)
	{
		FString Value;
		return Object.IsValid() && Object->TryGetStringField(FieldName, Value) && Value.StartsWith(Prefix);
	}

	static TSharedRef<FJsonObject> BuildModuleDescriptorFromCreationCenterManifest(const TSharedPtr<FJsonObject>& Manifest)
	{
		TSharedRef<FJsonObject> Module = MakeShared<FJsonObject>();
		FString ModuleId;
		ReadStringFieldAny(Manifest, { TEXT("module_id"), TEXT("moduleId"), TEXT("file_prefix"), TEXT("filePrefix") }, ModuleId);
		FString Category;
		ReadStringFieldAny(Manifest, { TEXT("category"), TEXT("module_kind"), TEXT("moduleKind"), TEXT("kind") }, Category);
		if (Category.IsEmpty()) { Category = TEXT("architecture_module"); }
		FString AssetPath;
		if (const TSharedPtr<FJsonObject>* Plan = nullptr; Manifest.IsValid() && Manifest->TryGetObjectField(TEXT("asset_storage_plan"), Plan) && Plan && Plan->IsValid())
		{
			ReadStringFieldAny(*Plan, { TEXT("ue_destination_path"), TEXT("ueDestinationPath") }, AssetPath);
		}
		if (AssetPath.IsEmpty()) { ReadStringFieldAny(Manifest, { TEXT("asset_path"), TEXT("asset_path_hint"), TEXT("ue_static_mesh_path") }, AssetPath); }
		if (ModuleId.IsEmpty()) { ModuleId = TEXT("architecture_module"); }
		Module->SetStringField(TEXT("module_id"), ModuleId);
		Module->SetStringField(TEXT("id"), ModuleId);
		Module->SetStringField(TEXT("kind"), Category);
		Module->SetStringField(TEXT("asset_path"), AssetPath);
		Module->SetStringField(TEXT("static_mesh"), AssetPath);
		Module->SetBoolField(TEXT("requires_collision"), true);
		Module->SetBoolField(TEXT("requires_nav"), true);
		Module->SetBoolField(TEXT("requires_lod"), true);
		if (const TSharedPtr<FJsonObject>* Sockets = nullptr; Manifest.IsValid() && Manifest->TryGetObjectField(TEXT("socket_contract"), Sockets) && Sockets && Sockets->IsValid())
		{
			Module->SetObjectField(TEXT("socket_contract"), *Sockets);
			const TArray<TSharedPtr<FJsonValue>>* SocketValues = nullptr;
			if ((*Sockets)->TryGetArrayField(TEXT("sockets"), SocketValues) && SocketValues)
			{
				Module->SetArrayField(TEXT("socket_types"), *SocketValues);
			}
		}
		return Module;
	}

	static bool RunWorldForgeArchitectureModuleContractValidate(const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("worldforge_architecture_module_contract_validate"));
		TSharedPtr<FJsonObject> Manifest;
		FString ManifestPath;
		if (!ResolveObjectOrPath(Arguments, TEXT("module_manifest"), TEXT("module_manifest_path"), Manifest, ManifestPath, Error))
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("status"), TEXT("module_contract_missing_manifest"));
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		TArray<FString> Failures;
		if (!JsonHasNonEmptyString(Manifest, TEXT("schema"))) { Failures.Add(TEXT("missing_schema")); }
		if (!JsonHasNonEmptyString(Manifest, TEXT("module_id"))) { Failures.Add(TEXT("missing_module_id")); }
		if (!Manifest->HasTypedField<EJson::Object>(TEXT("socket_contract"))) { Failures.Add(TEXT("missing_socket_contract")); }
		if (!Manifest->HasTypedField<EJson::Object>(TEXT("collision_nav"))) { Failures.Add(TEXT("missing_collision_nav")); }
		if (!Manifest->HasTypedField<EJson::Object>(TEXT("style_profile"))) { Failures.Add(TEXT("missing_style_profile")); }
		if (!Manifest->HasTypedField<EJson::Object>(TEXT("assembly_rules"))) { Failures.Add(TEXT("missing_assembly_rules")); }
		const TSharedPtr<FJsonObject>* StoragePlan = nullptr;
		if (!Manifest->TryGetObjectField(TEXT("asset_storage_plan"), StoragePlan) || !StoragePlan || !StoragePlan->IsValid())
		{
			Failures.Add(TEXT("missing_asset_storage_plan"));
		}
		else
		{
			if (!JsonStringStartsWith(*StoragePlan, TEXT("local_library_dir"), TEXT("Library/"))) { Failures.Add(TEXT("local_library_dir_not_under_library")); }
			if (!JsonStringStartsWith(*StoragePlan, TEXT("ue_destination_path"), TEXT("/Game/somolagent/"))) { Failures.Add(TEXT("ue_destination_path_not_under_somolagent")); }
		}
		const bool bOk = Failures.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("worldforge_architecture_module_contract_valid") : TEXT("worldforge_architecture_module_contract_failed"));
		Out->SetStringField(TEXT("module_manifest_path"), ManifestPath);
		Out->SetObjectField(TEXT("module_descriptor"), BuildModuleDescriptorFromCreationCenterManifest(Manifest));
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetArrayField(TEXT("required_sidecars"), StringArrayJson({ TEXT("module.manifest.json"), TEXT("sockets.json"), TEXT("collision_nav.json"), TEXT("style_profile.json"), TEXT("assembly_rules.json"), TEXT("worldforge_receipt.json") }));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%d"), *ManifestPath, Failures.Num())));
		if (!bOk)
		{
			Error = TEXT("WorldForge architecture module contract validation failed.");
			Out->SetStringField(TEXT("error"), Error);
			return false;
		}
		Summary = TEXT("WorldForge architecture module contract is valid.");
		return true;
	}

	static bool RunWorldForgeSettlementModuleRegistryImport(const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("worldforge_settlement_module_registry_import"));
		FString KitId;
		ReadStringFieldAny(Arguments, { TEXT("kit_id"), TEXT("kitId") }, KitId);
		if (KitId.IsEmpty()) { KitId = TEXT("creation_center_architecture_kit"); }
		TArray<TSharedPtr<FJsonValue>> ModuleRows;
		TArray<TSharedPtr<FJsonValue>> ModuleDescriptors;
		TArray<FString> Failures;
		const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
		if (Arguments->TryGetArrayField(TEXT("module_manifest_paths"), Paths) && Paths)
		{
			for (const TSharedPtr<FJsonValue>& PathValue : *Paths)
			{
				const FString Path = PathValue.IsValid() ? PathValue->AsString() : TEXT("");
				TSharedPtr<FJsonObject> Manifest;
				FString LoadError;
				if (!LoadJsonObjectFromPath(Path, Manifest, LoadError)) { Failures.Add(LoadError); continue; }
				TSharedRef<FJsonObject> Descriptor = BuildModuleDescriptorFromCreationCenterManifest(Manifest);
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("module_manifest_path"), Path);
				Row->SetObjectField(TEXT("module_descriptor"), Descriptor);
				ModuleRows.Add(MakeShared<FJsonValueObject>(Row));
				ModuleDescriptors.Add(MakeShared<FJsonValueObject>(Descriptor));
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* DirectModules = nullptr;
		if (ResolveModuleDescriptors(Arguments, DirectModules) && DirectModules)
		{
			for (const TSharedPtr<FJsonValue>& Value : *DirectModules) { if (Value.IsValid() && Value->AsObject().IsValid()) { ModuleDescriptors.Add(Value); } }
		}
		if (ModuleDescriptors.IsEmpty()) { Failures.Add(TEXT("missing_module_manifest_paths_or_module_descriptors")); }
		const bool bOk = Failures.IsEmpty();
		TSharedRef<FJsonObject> Registry = MakeShared<FJsonObject>();
		Registry->SetStringField(TEXT("schema"), TEXT("somol.worldforge.settlement_module_registry:v1"));
		Registry->SetStringField(TEXT("kit_id"), KitId);
		Registry->SetArrayField(TEXT("module_descriptors"), ModuleDescriptors);
		Registry->SetArrayField(TEXT("consumer_plugins"), StringArrayJson({ TEXT("SOMOLArchitectureCore"), TEXT("SOMOLRuntimeSettlement"), TEXT("SOMOLRuntimeContentBridge"), TEXT("SOMOLRuntimeNavigationClient") }));
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("worldforge_settlement_module_registry_ready") : TEXT("worldforge_settlement_module_registry_failed"));
		Out->SetObjectField(TEXT("module_registry"), Registry);
		Out->SetNumberField(TEXT("module_count"), ModuleDescriptors.Num());
		Out->SetArrayField(TEXT("module_rows"), ModuleRows);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%d:%d"), *KitId, ModuleDescriptors.Num(), Failures.Num())));
		if (!bOk) { Error = TEXT("WorldForge settlement module registry import failed."); Out->SetStringField(TEXT("error"), Error); return false; }
		Summary = FString::Printf(TEXT("WorldForge settlement module registry contains %d modules."), ModuleDescriptors.Num());
		return true;
	}

	static bool RunWorldForgeArchitectureKitImportToUe(const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		SetArchitectureReceiptBase(Out, TEXT("worldforge_architecture_kit_import_to_ue"));
		TSharedPtr<FJsonObject> Registry;
		FString RegistryPath;
		if (!ResolveObjectOrPath(Arguments, TEXT("module_registry"), TEXT("module_registry_path"), Registry, RegistryPath, Error))
		{
			const TSharedPtr<FJsonObject>* InlineRegistry = nullptr;
			if (Arguments->TryGetObjectField(TEXT("registry"), InlineRegistry) && InlineRegistry && InlineRegistry->IsValid()) { Registry = *InlineRegistry; Error.Reset(); }
		}
		const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
		if (!Registry.IsValid() || !Registry->TryGetArrayField(TEXT("module_descriptors"), Modules) || !Modules || Modules->IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false); Out->SetStringField(TEXT("status"), TEXT("worldforge_architecture_kit_import_missing_registry")); Error = Error.IsEmpty() ? TEXT("Missing module_registry with module_descriptors.") : Error; Out->SetStringField(TEXT("error"), Error); return false;
		}
		FString DestinationRoot;
		ReadStringFieldAny(Arguments, { TEXT("destination_root"), TEXT("ue_content_root") }, DestinationRoot);
		if (DestinationRoot.IsEmpty()) { DestinationRoot = TEXT("/Game/somolagent"); }
		TArray<TSharedPtr<FJsonValue>> ImportSteps;
		TArray<FString> Failures;
		for (const TSharedPtr<FJsonValue>& Value : *Modules)
		{
			const TSharedPtr<FJsonObject> Module = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Module.IsValid()) { Failures.Add(TEXT("invalid_module_descriptor")); continue; }
			FString ModuleId; ReadStringFieldAny(Module, { TEXT("module_id"), TEXT("id") }, ModuleId);
			FString AssetPath = ReadModuleAssetPath(Module);
			if (AssetPath.IsEmpty()) { AssetPath = FString::Printf(TEXT("%s/architecture_module/%s"), *DestinationRoot, *ModuleId); }
			TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("module_id"), ModuleId);
			Step->SetStringField(TEXT("destination_path"), AssetPath);
			Step->SetStringField(TEXT("import_tool"), TEXT("texture_studio_import_static_model_to_ue"));
			Step->SetArrayField(TEXT("post_import_tools"), StringArrayJson({ TEXT("architecture_module_validate"), TEXT("architecture_socket_contract_validate"), TEXT("architecture_collision_contract_validate") }));
			ImportSteps.Add(MakeShared<FJsonValueObject>(Step));
		}
		bool bExecute = false; Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		const bool bOk = Failures.IsEmpty() && !ImportSteps.IsEmpty();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? (bExecute ? TEXT("worldforge_architecture_kit_import_plan_ready_execute_delegated") : TEXT("worldforge_architecture_kit_import_plan_ready")) : TEXT("worldforge_architecture_kit_import_plan_failed"));
		Out->SetBoolField(TEXT("execute_requested"), bExecute);
		Out->SetStringField(TEXT("destination_root"), DestinationRoot);
		Out->SetArrayField(TEXT("import_steps"), ImportSteps);
		Out->SetArrayField(TEXT("failures"), StringArrayJson(Failures));
		Out->SetArrayField(TEXT("required_followup_receipts"), StringArrayJson({ TEXT("static_model_import_receipt"), TEXT("architecture_module_validate"), TEXT("architecture_socket_contract_validate"), TEXT("architecture_collision_contract_validate"), TEXT("worldforge_settlement_module_registry_import") }));
		Out->SetStringField(TEXT("receipt_hash"), MakeReceiptHash(FString::Printf(TEXT("%s:%d:%d"), *DestinationRoot, ImportSteps.Num(), Failures.Num())));
		if (!bOk) { Error = TEXT("WorldForge architecture kit import plan failed."); Out->SetStringField(TEXT("error"), Error); return false; }
		Summary = FString::Printf(TEXT("WorldForge architecture kit import plan contains %d module imports."), ImportSteps.Num());
		return true;
	}

	static void RegisterWorldForgeArchitectureModuleContractValidate(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("worldforge_architecture_module_contract_validate");
		Def.Description = TEXT("Validate Creation Center architecture module sidecar manifest for WorldForge consumption; fail-closed on missing sockets, collision/nav, storage paths, or assembly rules.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({ {TEXT("module_manifest_path"), FSololmcpSchemaBuilder::String(TEXT("Path to module.manifest.json."))}, {TEXT("module_manifest"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Inline module manifest."))}, {TEXT("check_sockets"), FSololmcpSchemaBuilder::Boolean(TEXT("Reserved hard gate; default true."))}, {TEXT("check_collision"), FSololmcpSchemaBuilder::Boolean(TEXT("Reserved hard gate; default true."))}, {TEXT("check_nav"), FSololmcpSchemaBuilder::Boolean(TEXT("Reserved hard gate; default true."))}, {TEXT("check_scale"), FSololmcpSchemaBuilder::Boolean(TEXT("Reserved hard gate; default true."))}, {TEXT("check_materials"), FSololmcpSchemaBuilder::Boolean(TEXT("Reserved hard gate; default true."))} });
		Def.CacheTtlSeconds = 0;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) { return RunWorldForgeArchitectureModuleContractValidate(Arguments, Out, Summary, Error); };
		Registry.Register(Def);
	}

	static void RegisterWorldForgeSettlementModuleRegistryImport(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("worldforge_settlement_module_registry_import");
		Def.Description = TEXT("Import Creation Center architecture sidecars into a WorldForge settlement module registry for ArchitectureCore, Settlement, ContentBridge, and NavigationClient.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({ {TEXT("kit_id"), FSololmcpSchemaBuilder::String(TEXT("Architecture kit id."))}, {TEXT("module_manifest_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Path to module.manifest.json")), TEXT("Module sidecar manifest paths."))}, {TEXT("module_descriptors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Inline module descriptors."))}, {TEXT("kit_manifest"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Inline kit manifest."))}, {TEXT("validate_only"), FSololmcpSchemaBuilder::Boolean(TEXT("Return registry without mutating runtime state."))}, {TEXT("fail_on_missing_sidecars"), FSololmcpSchemaBuilder::Boolean(TEXT("Reserved hard gate; default true."))} });
		Def.CacheTtlSeconds = 0;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) { return RunWorldForgeSettlementModuleRegistryImport(Arguments, Out, Summary, Error); };
		Registry.Register(Def);
	}

	static void RegisterWorldForgeArchitectureKitImportToUe(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("worldforge_architecture_kit_import_to_ue");
		Def.Description = TEXT("Build the MCP-executable UE import plan for a Creation Center architecture kit registry, including validation and collision/nav follow-up receipts.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({ {TEXT("module_registry"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Registry from worldforge_settlement_module_registry_import."))}, {TEXT("registry"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Alias for module_registry."))}, {TEXT("module_registry_path"), FSololmcpSchemaBuilder::String(TEXT("Optional registry JSON path."))}, {TEXT("destination_root"), FSololmcpSchemaBuilder::String(TEXT("UE destination root, defaults /Game/somolagent."))}, {TEXT("replace_existing"), FSololmcpSchemaBuilder::Boolean(TEXT("Forwarded to import execution queue."))}, {TEXT("generate_material_instances"), FSololmcpSchemaBuilder::Boolean(TEXT("Forwarded to import execution queue."))}, {TEXT("generate_lods"), FSololmcpSchemaBuilder::Boolean(TEXT("Forwarded to import execution queue."))}, {TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("When true, request execution by downstream import tools; this tool returns the safe command plan."))} });
		Def.CacheTtlSeconds = 0;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) { return RunWorldForgeArchitectureKitImportToUe(Arguments, Out, Summary, Error); };
		Registry.Register(Def);
	}
static void RegisterModuleValidate(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_module_validate");
		Def.Description = TEXT("Validate one modular architecture StaticMesh for bounds, LOD, material slots, sockets, and simple collision.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("UStaticMesh content path."))}
		}, {TEXT("asset_path")});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureModuleValidate(Context, Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterSocketContractValidate(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_socket_contract_validate");
		Def.Description = TEXT("Validate required modular architecture sockets against a StaticMesh.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("UStaticMesh content path."))},
			{TEXT("required_sockets"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Socket name")), TEXT("Required socket names."))}
		}, {TEXT("asset_path"), TEXT("required_sockets")});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureSocketContractValidate(Context, Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterKitCoverageAudit(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_kit_coverage_audit");
		Def.Description = TEXT("Audit whether a modular architecture kit covers required semantic module kinds.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("module_descriptors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Module descriptors with at least a kind field."))},
			{TEXT("required_kinds"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Module kind")), TEXT("Required module kinds; defaults to wall, door, floor, roof, stair."))}
		}, {TEXT("module_descriptors")});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureKitCoverageAudit(Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterKitManifestValidate(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_kit_manifest_validate");
		Def.Description = TEXT("Validate every module descriptor in a ModularKitManifest against loaded StaticMesh asset, material, socket, and collision evidence.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("module_descriptors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Module descriptors with kind and asset_path/static_mesh."))}
		}, {TEXT("module_descriptors")});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureKitManifestValidate(Context, Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterReferenceInputClassify(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_reference_input_classify");
		Def.Description = TEXT("Classify text/image/document user input for Reference-to-Kit architecture generation. Text-only input requires exactly four reference candidates and user selection; user images become primary references.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("prompt"), FSololmcpSchemaBuilder::String(TEXT("User text prompt."))},
			{TEXT("attachments"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("User files/images/documents."))}
		});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureReferenceInputClassify(Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterReferenceCandidatesGenerate(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_reference_candidates_generate");
		Def.Description = TEXT("Generate the fail-closed 4-up reference-image candidate prompt contract for text-only architecture kit creation.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("prompt"), FSololmcpSchemaBuilder::String(TEXT("Text prompt to turn into four reference candidates."))},
			{TEXT("candidate_count"), FSololmcpSchemaBuilder::Integer(TEXT("Must be 4 for the hard gate."))}
		}, {TEXT("prompt")});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureReferenceCandidatesGenerate(Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterReferenceSetBuild(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_reference_set_build");
		Def.Description = TEXT("Build a selected architecture reference set from user images or selected 4-up candidates before style analysis and kit generation.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("references"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Selected reference objects or user image attachments."))},
			{TEXT("selected_ids"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Selected reference candidate id")), TEXT("Selected candidate ids."))},
			{TEXT("uses_user_images_as_primary"), FSololmcpSchemaBuilder::Boolean(TEXT("True when user supplied images are primary references."))}
		});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureReferenceSetBuild(Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterReferenceStyleAnalyze(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_reference_style_analyze");
		Def.Description = TEXT("Analyze selected architecture references into style_id, tags, silhouette, roof language, wall language, palette, and modularizable elements.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("prompt"), FSololmcpSchemaBuilder::String(TEXT("Original prompt or style notes."))},
			{TEXT("reference_set"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Reference set from architecture_reference_set_build."))}
		});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureReferenceStyleAnalyze(Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterKitManifestGenerate(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_kit_manifest_generate");
		Def.Description = TEXT("Generate a deterministic modular architecture kit manifest from style analysis: module descriptors, dimensions, sockets, assembly templates, collision/nav/LOD/HLOD flags.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("style_id"), FSololmcpSchemaBuilder::String(TEXT("Style id from architecture_reference_style_analyze."))},
			{TEXT("kit_id"), FSololmcpSchemaBuilder::String(TEXT("Optional kit id."))},
			{TEXT("grid_cm"), FSololmcpSchemaBuilder::Number(TEXT("Module grid size, clamped 10..1000 cm."))},
			{TEXT("floor_height_cm"), FSololmcpSchemaBuilder::Number(TEXT("Floor height, clamped 180..800 cm."))}
		});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureKitManifestGenerate(Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterModuleDagGenerate(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_module_dag_generate");
		Def.Description = TEXT("Generate Creation Center module-production DAG tasks from a modular architecture kit manifest.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("kit_id"), FSololmcpSchemaBuilder::String(TEXT("Kit id."))},
			{TEXT("module_descriptors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Module descriptors from architecture_kit_manifest_generate."))}
		}, {TEXT("module_descriptors")});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureModuleDagGenerate(Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterCollisionContractValidate(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_collision_contract_validate");
		Def.Description = TEXT("Validate a modular architecture StaticMesh against collision, doorway, and traversal clearance contract fields.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("UStaticMesh content path."))},
			{TEXT("requires_simple_collision"), FSololmcpSchemaBuilder::Boolean(TEXT("Require simple collision. Default true."))},
			{TEXT("min_door_width_cm"), FSololmcpSchemaBuilder::Number(TEXT("Optional minimum doorway width."))},
			{TEXT("min_door_height_cm"), FSololmcpSchemaBuilder::Number(TEXT("Optional minimum doorway height."))}
		}, {TEXT("asset_path")});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureCollisionContractValidate(Context, Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterSettlementFootprintAudit(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_settlement_footprint_audit");
		Def.Description = TEXT("Audit settlement building footprints for missing coordinates, invalid sizes, overlaps, and spacing conflicts before city batch writes.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("settlement_plan"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Settlement plan with buildings[]."))},
			{TEXT("buildings"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Optional direct building array."))},
			{TEXT("min_spacing_cm"), FSololmcpSchemaBuilder::Number(TEXT("Minimum center spacing for non-overlapping close buildings."))},
			{TEXT("allow_overlap"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow intentional overlap; defaults false."))}
		}, {});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureSettlementFootprintAudit(Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterStaticMeshCollisionRepair(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_staticmesh_collision_repair");
		Def.Description = TEXT("Repair architecture module UStaticMesh assets by writing simple collision shapes with readback receipts.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Single UStaticMesh object path."))},
			{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("UStaticMesh object path")), TEXT("Batch UStaticMesh object paths."))},
			{TEXT("collision_type"), FSololmcpSchemaBuilder::String(TEXT("box|sphere|capsule|convex|obb. Default box."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false; true writes BodySetup simple collision."))},
			{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean(TEXT("Save repaired mesh assets after execution. Default false."))},
			{TEXT("max_assets"), FSololmcpSchemaBuilder::Integer(TEXT("Batch guard. Default 64."))}
		});
		Def.CacheTtlSeconds = 0;
		Def.Execute = [](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureStaticMeshCollisionRepair(Context, Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterDeploymentMetadataApply(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_deployment_metadata_apply");
		Def.Description = TEXT("Apply architecture deployment metadata to assembled actors: deployment/district/runtime-grid tags and optional HLODLayer readback.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false; true mutates actor tags/HLODLayer."))},
			{TEXT("assembly_id"), FSololmcpSchemaBuilder::String(TEXT("SOMOLArchitectureAssembly id used to resolve actors."))},
			{TEXT("actor_labels"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Actor label/name")), TEXT("Actor labels to mutate."))},
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Assembly/readback receipt with spawned_actors."))},
			{TEXT("deployment_id"), FSololmcpSchemaBuilder::String(TEXT("Deployment id tag to write."))},
			{TEXT("district_id"), FSololmcpSchemaBuilder::String(TEXT("Settlement district id tag to write."))},
			{TEXT("data_layer_name"), FSololmcpSchemaBuilder::String(TEXT("DataLayer intent tag; use world_create_data_layer_actor_membership_apply for native membership."))},
			{TEXT("runtime_grid"), FSololmcpSchemaBuilder::String(TEXT("Runtime grid intent tag."))},
			{TEXT("hlod_layer_path"), FSololmcpSchemaBuilder::String(TEXT("Optional UHLODLayer asset path to set via actor HLODLayer property."))}
		});
		Def.CacheTtlSeconds = 0;
		Def.Execute = [](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureDeploymentMetadataApply(Context, Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterHlodBuildDispatch(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_hlod_build_dispatch");
		Def.Description = TEXT("Dispatch architecture HLOD generation/rebuild through World Partition console commands and return async receipt evidence.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false; true dispatches the HLOD command."))},
			{TEXT("mode"), FSololmcpSchemaBuilder::String(TEXT("changed|all|generate|rebuild. Default changed."))},
			{TEXT("hlod_layer_index"), FSololmcpSchemaBuilder::Integer(TEXT("Layer index for wp.HLOD.Generate. Default 0."))}
		});
		Def.CacheTtlSeconds = 0;
		Def.Execute = [](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureHlodBuildDispatch(Context, Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterNavmeshPathSample(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_navmesh_path_sample");
		Def.Description = TEXT("Sample real UE NavigationSystem paths between architecture entrances, rooms, bridges, or gate nodes.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("path_samples"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}, {}, TEXT("Object with start_cm and end_cm vectors.")), TEXT("Explicit path samples."))},
			{TEXT("interior_nav_graph"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("InteriorNavGraph with nodes/links; links become path samples when explicit samples are absent."))},
			{TEXT("max_samples"), FSololmcpSchemaBuilder::Integer(TEXT("Sample guard. Default 64."))},
			{TEXT("require_complete_paths"), FSololmcpSchemaBuilder::Boolean(TEXT("Fail partial paths. Default true."))}
		});
		Def.CacheTtlSeconds = 0;
		Def.Execute = [](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureNavmeshPathSample(Context, Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterProductionGateValidate(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_production_gate_validate");
		Def.Description = TEXT("Fail-closed final gate for modular architecture production receipts: assembly, collision, reachability, nav, DataLayer, HLOD, save, reload, and screenshot evidence.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("prior_receipts"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}, {}, TEXT("Prior receipt object")), TEXT("Receipts to validate."))},
			{TEXT("receipts"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}, {}, TEXT("Alias for prior_receipts.")), TEXT("Receipts to validate."))},
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Single receipt alias."))},
			{TEXT("required_gates"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Gate id")), TEXT("Optional gates: assembly, collision, reachability, nav, data_layer, hlod, save, reload, screenshot."))},
			{TEXT("require_worldforge_chain"), FSololmcpSchemaBuilder::Boolean(TEXT("When true, require nav, DataLayer, HLOD, save, reload, and screenshot gates too."))}
		});
		Def.CacheTtlSeconds = 0;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureProductionGateValidate(Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterCityBatchPlan(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_city_batch_plan");
		Def.Description = TEXT("Split a village, town, city, or metropolis settlement plan into safe MCP execution batches with per-batch assembly, collision, nav, and follow-up gates.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("settlement_plan"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Settlement plan with buildings[] from settlement_layout_plan_build."))},
			{TEXT("settlement_plan_pages"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}, {}, TEXT("Layout page or wrapper containing settlement_plan.")), TEXT("Multiple paged layout results; buildings are deduplicated by id and combined before batching."))},
			{TEXT("buildings"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Shortcut for settlement_plan.buildings."))},
			{TEXT("plan_id"), FSololmcpSchemaBuilder::String(TEXT("Optional deterministic batch plan id."))},
			{TEXT("settlement_id"), FSololmcpSchemaBuilder::String(TEXT("Optional settlement id override."))},
			{TEXT("max_buildings_per_batch"), FSololmcpSchemaBuilder::Integer(TEXT("Batch guard. Default 64, clamped 1..256."))},
			{TEXT("max_modules_per_batch"), FSololmcpSchemaBuilder::Integer(TEXT("Estimated module guard. Default 1200."))},
			{TEXT("max_parallel_write_batches"), FSololmcpSchemaBuilder::Integer(TEXT("Write-lane group guard. Default 4."))}
		});
		Def.CacheTtlSeconds = 5;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureCityBatchPlan(Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterCityScaleGateValidate(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("architecture_city_scale_gate_validate");
		Def.Description = TEXT("Fail-closed configurable production-scale acceptance gate for large city generation; the target count is a minimum proof threshold, never a global city-size limit.");
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("city_batch_plan"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Batch plan returned by architecture_city_batch_plan."))},
			{TEXT("batch_plan"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Alias for city_batch_plan."))},
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Receipt that may contain city_batch_plan."))},
			{TEXT("prior_receipts"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}, {}, TEXT("Prior receipt object")), TEXT("Receipts to validate."))},
			{TEXT("receipts"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}, {}, TEXT("Alias for prior_receipts.")), TEXT("Receipts to validate."))},
			{TEXT("acceptance_target_building_count"), FSololmcpSchemaBuilder::Integer(TEXT("Minimum total building count required by this acceptance run; for example 2000. This is not a maximum city-size limit."))},
			{TEXT("min_building_count"), FSololmcpSchemaBuilder::Integer(TEXT("Backward-compatible alias for acceptance_target_building_count. Default 1."))},
			{TEXT("max_buildings_per_batch"), FSololmcpSchemaBuilder::Integer(TEXT("Per-batch building guard. Default 256."))},
			{TEXT("max_modules_per_batch"), FSololmcpSchemaBuilder::Integer(TEXT("Per-batch estimated module guard. Default 10000."))},
			{TEXT("require_receipts"), FSololmcpSchemaBuilder::Boolean(TEXT("When true, require batch/global receipts. Default true."))},
			{TEXT("required_global_gates"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Gate id")), TEXT("Optional gates: nav, data_layer, hlod, save, reload, screenshot."))}
		});
		Def.CacheTtlSeconds = 0;
		Def.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return RunArchitectureCityScaleGateValidate(Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}

	static void RegisterReceiptGatedTool(
		FSololmcpToolRegistry& Registry,
		const TCHAR* Name,
		const TCHAR* Description,
		bool bMutating,
		const TArray<FString>& RequiredReceipts)
	{
		FSololmcpToolDefinition Def;
		Def.Name = Name;
		Def.Description = Description;
		Def.InputSchema = GenericArchitectureInputSchema(bMutating);
		Def.CacheTtlSeconds = bMutating ? 0 : 5;
		Def.Execute = [ToolName = FString(Name), RequiredReceipts, bMutating](
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& Out,
			FString& Summary,
			FString& Error)
		{
			return RunReceiptGatedArchitectureTool(Context, ToolName, RequiredReceipts, bMutating, Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}
}

void RegisterArchitectureTools(FSololmcpToolRegistry& Registry)
{
	using namespace ArchitectureTools;

	RegisterModuleValidate(Registry);
	RegisterSocketContractValidate(Registry);
	RegisterKitCoverageAudit(Registry);
	RegisterKitManifestValidate(Registry);
	RegisterReferenceInputClassify(Registry);
	RegisterReferenceCandidatesGenerate(Registry);
	RegisterReferenceSetBuild(Registry);
	RegisterReferenceStyleAnalyze(Registry);
	RegisterKitManifestGenerate(Registry);
	RegisterModuleDagGenerate(Registry);
	RegisterCollisionContractValidate(Registry);
	RegisterSettlementFootprintAudit(Registry);
	RegisterStaticMeshCollisionRepair(Registry);
	RegisterDeploymentMetadataApply(Registry);
	RegisterHlodBuildDispatch(Registry);
	RegisterNavmeshPathSample(Registry);
	RegisterProductionGateValidate(Registry);
	RegisterCityBatchPlan(Registry);
	RegisterCityScaleGateValidate(Registry);
#if SOMOLMCP_WITH_WORLDFORGE
	RegisterWorldForgeArchitectureModuleContractValidate(Registry);
	RegisterWorldForgeSettlementModuleRegistryImport(Registry);
	RegisterWorldForgeArchitectureKitImportToUe(Registry);
#endif

	RegisterReceiptGatedTool(
		Registry,
		TEXT("architecture_assembly_plan_build"),
		TEXT("Build a modular building assembly plan with room/floor/module/socket/transform receipts."),
		false,
		{ TEXT("ArchitectureRecipe"), TEXT("ModularKitManifest"), TEXT("SocketContract") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("architecture_modular_assembly_plan"),
		TEXT("Convert settlement/building footprint contracts into executable modular building module transforms."),
		false,
		{ TEXT("SettlementPlan"), TEXT("ModularKitManifest"), TEXT("SocketContract") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("architecture_assembly_execute"),
		TEXT("Execute a modular building assembly plan by spawning StaticMesh module actors with readback receipts."),
		true,
		{ TEXT("assembly_plan"), TEXT("target_guard"), TEXT("resource_lock"), TEXT("rollback_snapshot") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("architecture_assembly_readback"),
		TEXT("Read back assembled building actor/component/socket transform evidence."),
		false,
		{ TEXT("assembly_receipt"), TEXT("transform_hash"), TEXT("component_count") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("architecture_collision_generate"),
		TEXT("Generate or repair aggregate component collision for an assembled modular building."),
		true,
		{ TEXT("CollisionContract"), TEXT("assembly_readback"), TEXT("rollback_snapshot") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("architecture_collision_audit"),
		TEXT("Audit assembled building collision for blocked entrances, invisible walls, overlap, and traversal clearance."),
		false,
		{ TEXT("collision_contract"), TEXT("assembly_readback") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("architecture_interior_nav_build"),
		TEXT("Build an interior navigation graph/navmesh contract for rooms, floors, stairs, bridges, and wall walks; execute=true updates nav relevance and dirties navigation."),
		true,
		{ TEXT("InteriorNavGraph"), TEXT("collision_audit"), TEXT("target_guard") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("architecture_navlink_generate"),
		TEXT("Generate NavLinkProxy actors for stairs, ramps, ladders, elevators, drawbridges, and gates."),
		true,
		{ TEXT("interior_nav_graph"), TEXT("collision_audit") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("architecture_reachability_audit"),
		TEXT("Audit road-to-entrance, entrance-to-room, and floor-to-floor reachability for assembled architecture."),
		false,
		{ TEXT("InteriorNavGraph"), TEXT("assembly_readback"), TEXT("collision_audit") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("settlement_layout_plan_build"),
		TEXT("Build settlement road, parcel, district, building, entrance, DataLayer, and HLOD layout contracts."),
		false,
		{ TEXT("WorldRecipe"), TEXT("TerrainBuildableRegions"), TEXT("Hydrology"), TEXT("ArchitectureRecipe") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("settlement_road_to_entrance_validate"),
		TEXT("Validate every settlement building entrance is connected to a road, plaza, or parcel path."),
		false,
		{ TEXT("SettlementGraph"), TEXT("EntranceNodes"), TEXT("RoadGraph") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("bridge_passability_audit"),
		TEXT("Audit wood, drawbridge, and stone bridge visual/collision/nav continuity."),
		false,
		{ TEXT("BridgePlan"), TEXT("collision_audit"), TEXT("nav_graph") });
	RegisterReceiptGatedTool(
		Registry,
		TEXT("fortress_gate_nav_validate"),
		TEXT("Validate gate, drawbridge, wall-walk, tower, and open/closed fortress navigation states."),
		false,
		{ TEXT("FortressPlan"), TEXT("gate_state"), TEXT("nav_links") });
}
}


