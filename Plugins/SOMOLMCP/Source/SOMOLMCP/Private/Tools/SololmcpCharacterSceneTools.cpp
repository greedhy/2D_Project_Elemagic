// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP v3.4 — Character Scene Tools
// One-click character setup, ground snap, collision-aware placement, batch spawn, animation listing
#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Selection.h"

namespace UE::SOMOLMCP{
using SB=FSololmcpSchemaBuilder;
static void GetSelectedActors(TArray<AActor*>& Out){
	if(USelection* Sel=GEditor->GetSelectedActors())for(FSelectionIterator It(*Sel);It;++It)if(auto*A=Cast<AActor>(*It))Out.Add(A);
}

static bool SnapToGround(UWorld*W,const FVector&In,FVector&Out,float Up=5000.f,float Down=5000.f){
	if(!W)return false;
	FHitResult Hit;FCollisionQueryParams P(SCENE_QUERY_STAT(Snap),true);
	if(W->LineTraceSingleByChannel(Hit,In+FVector(0,0,Up),In-FVector(0,0,Down),ECC_WorldStatic,P)){Out=Hit.Location;return true;}
	Out=In;return false;
}
static FBox ActorBounds(AActor*A){
	FBox B(ForceInit);if(!A)return B;
	TArray<UPrimitiveComponent*> P;A->GetComponents<UPrimitiveComponent>(P);
	for(auto*C:P){if(C&&C->IsRegistered())B+=C->Bounds.GetBox();}return B;
}
static bool ActorsOverlap(AActor*A,AActor*B,float Tol=5.f){
	FBox BA=ActorBounds(A),BB=ActorBounds(B);
	if(!BA.IsValid||!BB.IsValid)return false;
	return BA.ExpandBy(-Tol).Intersect(BB.ExpandBy(-Tol));
}

// Resolve actor by name using 3-tier matching: PathName > GetName > ActorLabel
// Mirrors FSololmcpEditorServices::FindActorByLabelOrName logic
static AActor* ResolveActor(UWorld*W,const FString& Id){
	if(!W)return nullptr;
	// Tier 1: PathName (globally unique)
	for(TActorIterator<AActor>It(W);It;++It){if((*It)->GetPathName()==Id)return *It;}
	// Tier 2: GetName (unique within World)
	for(TActorIterator<AActor>It(W);It;++It){if((*It)->GetName()==Id)return *It;}
	// Tier 3: ActorLabel (may not be unique, return first)
	for(TActorIterator<AActor>It(W);It;++It){if((*It)->GetActorLabel()==Id)return *It;}
	return nullptr;
}

// Resolve multiple actors from actor_names array using 3-tier matching
static void ResolveActors(UWorld*W,const TArray<TSharedPtr<FJsonValue>>& Names,TArray<AActor*>& Out){
	for(const auto&V:Names){
		FString Nm=V->AsString();
		if(AActor*A=ResolveActor(W,Nm))Out.Add(A);
	}
}

void RegisterCharacterSceneTools(FSololmcpToolRegistry&R){

// 1. character_setup_full — one-click pipeline
R.Register({TEXT("character_setup_full"),
TEXT("One-click full character setup: load mesh, validate animation, create AnimBP, spawn character on ground with collision avoidance. Returns setup info and next steps."),
SB::Object({
	{TEXT("skeletal_mesh_path"),SB::String(TEXT("SkeletalMesh asset path"))},
	{TEXT("animation_asset_path"),SB::String(TEXT("Idle animation sequence path"))},
	{TEXT("skeleton_path"),SB::String(TEXT("Optional explicit USkeleton asset path. Falls back to mesh's skeleton, then anim's skeleton."))},
	{TEXT("location_x"),SB::Number(TEXT("X (default 0)"))},{TEXT("location_y"),SB::Number(TEXT("Y (default 0)"))},{TEXT("location_z"),SB::Number(TEXT("Z (default 0, auto-snap if 0)"))},
	{TEXT("rotation_yaw"),SB::Number(TEXT("Yaw degrees (default 0)"))},
	{TEXT("actor_label"),SB::String(TEXT("Actor label (auto if omitted)"))},
	{TEXT("anim_bp_output_path"),SB::String(TEXT("AnimBP output path (auto if omitted)"))},
	{TEXT("snap_to_ground"),SB::Boolean(TEXT("Snap to ground (default true)"))},
	{TEXT("avoid_overlap"),SB::Boolean(TEXT("Avoid overlapping actors (default true)"))},
	{TEXT("spawn_as_character"),SB::Boolean(TEXT("v12-rd3: spawn as ACharacter (Pawn) instead of ASkeletalMeshActor so AI controllers can possess + drive it. Default false (legacy)."))}
},{TEXT("skeletal_mesh_path"),TEXT("animation_asset_path")}),
[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
	FString MP;if(!A->TryGetStringField(TEXT("skeletal_mesh_path"),MP)){Er=TEXT("Missing skeletal_mesh_path");return false;}
	FString AP;if(!A->TryGetStringField(TEXT("animation_asset_path"),AP)){Er=TEXT("Missing animation_asset_path");return false;}
	double LX=A->HasTypedField<EJson::Number>(TEXT("location_x"))?A->GetNumberField(TEXT("location_x")):0;
	double LY=A->HasTypedField<EJson::Number>(TEXT("location_y"))?A->GetNumberField(TEXT("location_y")):0;
	double LZ=A->HasTypedField<EJson::Number>(TEXT("location_z"))?A->GetNumberField(TEXT("location_z")):0;
	double RY=A->HasTypedField<EJson::Number>(TEXT("rotation_yaw"))?A->GetNumberField(TEXT("rotation_yaw")):0;
	FString Label=A->HasField(TEXT("actor_label"))?A->GetStringField(TEXT("actor_label")):FString();
	FString ABPPath=A->HasField(TEXT("anim_bp_output_path"))?A->GetStringField(TEXT("anim_bp_output_path")):FString();
	bool bSnap=A->HasField(TEXT("snap_to_ground"))?A->GetBoolField(TEXT("snap_to_ground")):true;
	bool bAvoid=A->HasField(TEXT("avoid_overlap"))?A->GetBoolField(TEXT("avoid_overlap")):true;
	bool bSpawnAsChar=A->HasField(TEXT("spawn_as_character"))?A->GetBoolField(TEXT("spawn_as_character")):false;

	USkeletalMesh*Mesh=Cast<USkeletalMesh>(Cx.Services.LoadAsset(MP,Er));if(!Mesh){Er=TEXT("Mesh not found: ")+MP;return false;}
	UAnimSequence*Anim=Cast<UAnimSequence>(Cx.Services.LoadAsset(AP,Er));if(!Anim){Er=TEXT("Anim not found: ")+AP;return false;}

	// FIX (v12-rd2): skeleton resolution with fallbacks. Mesh->GetSkeleton() can
	// return null when the skeleton package isn't loaded yet (which happens for
	// imported assets MCP touches without ever opening their editor). Try in order:
	//   1. user-provided skeleton_path (explicit override)
	//   2. Mesh->GetSkeleton()
	//   3. Anim->GetSkeleton() (the matching anim must reference the right skeleton)
	const USkeleton* Skel = nullptr;
	FString UserSkelPath; FString SkelLoadErr;
	if (A->TryGetStringField(TEXT("skeleton_path"), UserSkelPath) && !UserSkelPath.IsEmpty())
	{
		Skel = Cast<USkeleton>(Cx.Services.LoadAsset(UserSkelPath, SkelLoadErr));
	}
	if (!Skel) { Skel = Mesh->GetSkeleton(); }
	if (!Skel) { Skel = Anim->GetSkeleton(); }
	if (!Skel)
	{
		Er = FString::Printf(TEXT("No skeleton resolvable. Mesh '%s' has no embedded skeleton, anim '%s' has no skeleton, and no skeleton_path was given. Pass skeleton_path explicitly."), *MP, *AP);
		return false;
	}
	O->SetStringField(TEXT("skeleton_resolved_via"),
		!UserSkelPath.IsEmpty() ? TEXT("skeleton_path") :
		(Mesh->GetSkeleton() == Skel ? TEXT("mesh") : TEXT("anim")));

	// Auto AnimBP path
	if(ABPPath.IsEmpty()){FString Pkg=FPackageName::GetLongPackagePath(MP);ABPPath=Pkg/TEXT("ABP_")+Mesh->GetName();}
	FString ABPName=FPackageName::GetLongPackageAssetName(ABPPath);
	UPackage*Pkg=CreatePackage(*ABPPath);if(!Pkg){Er=TEXT("Package failed");return false;}
	UAnimBlueprint*ABP=NewObject<UAnimBlueprint>(Pkg,*ABPName,RF_Public|RF_Standalone);
	if(!ABP){Er=TEXT("AnimBP create failed");return false;}
	FAssetRegistryModule::AssetCreated(ABP);ABP->MarkPackageDirty();

	UWorld*W=GEditor->GetEditorWorldContext().World();if(!W){Er=TEXT("No world");return false;}
	FVector Loc((float)LX,(float)LY,(float)LZ);
	FVector GL;
	if(bSnap&&SnapToGround(W,Loc,GL))Loc=GL;

	FActorSpawnParameters SP;SP.bAllowDuringConstructionScript=true;

	// FIX (v12-rd3): branch on spawn_as_character to support AI workflows.
	// ASkeletalMeshActor cannot be possessed by AIController; ACharacter can.
	AActor* Act = nullptr;
	USkeletalMeshComponent* SkComp = nullptr;
	if (bSpawnAsChar)
	{
		ACharacter* Ch = W->SpawnActor<ACharacter>(ACharacter::StaticClass(), Loc, FRotator(0,(float)RY,0), SP);
		if (!Ch) { Er = TEXT("Character spawn failed"); return false; }
		Act = Ch;
		SkComp = Ch->GetMesh();
		if (SkComp)
		{
			// Center the skeletal mesh inside the capsule (Quinn anchor at feet)
			SkComp->SetRelativeLocation(FVector(0, 0, -88));
			SkComp->SetRelativeRotation(FRotator(0, -90, 0));
		}
		// Set capsule reasonable size
		if (UCapsuleComponent* Cap = Ch->GetCapsuleComponent())
		{
			Cap->SetCapsuleHalfHeight(96.0f);
			Cap->SetCapsuleRadius(42.0f);
		}
		// Auto-spawn AIController so this Character can be driven by behavior trees
		Ch->AIControllerClass = AAIController::StaticClass();
		Ch->AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
		Ch->SpawnDefaultController();
	}
	else
	{
		ASkeletalMeshActor* SmAct = W->SpawnActor<ASkeletalMeshActor>(ASkeletalMeshActor::StaticClass(), Loc, FRotator(0,(float)RY,0), SP);
		if (!SmAct) { Er = TEXT("Spawn failed"); return false; }
		Act = SmAct;
		SkComp = SmAct->GetSkeletalMeshComponent();
	}
	if (SkComp) { SkComp->SetSkeletalMesh(Mesh); }

	// Overlap avoidance
	bool bHadOv=false;
	if(bAvoid){
		for(int32 i=0;i<8;i++){
			bool Ov=false;
			for(TActorIterator<AActor>It(W);It&&!Ov;++It){if(*It!=Act&&ActorsOverlap(Act,*It,2.f))Ov=true;}
			if(!Ov)break;
			bHadOv=true;
			float S=150.f;FVector Off;
			switch(i%4){case 0:Off=FVector(S,0,0);break;case 1:Off=FVector(0,S,0);break;case 2:Off=FVector(-S,0,0);break;default:Off=FVector(0,-S,0);break;}
			Act->SetActorLocation(Act->GetActorLocation()+Off);
			if(bSnap){FVector T;if(SnapToGround(W,Act->GetActorLocation(),T))Act->SetActorLocation(T);}
		}
	}
	bool bStillOverlaps=false;
	if(bAvoid){for(TActorIterator<AActor>It(W);It&&!bStillOverlaps;++It){if(*It!=Act&&ActorsOverlap(Act,*It,2.f))bStillOverlaps=true;}}
	if(!Label.IsEmpty())Act->SetActorLabel(Label);
	else Act->SetActorLabel(TEXT("Char_")+Mesh->GetName());

	bool bAnimOK=false;
	if (SkComp)  // FIX (v12-rd3): use captured SkComp (works for both spawn paths)
	{
		// FIX (v12-rd4): the empty AnimBP we created has no graph, so even after
		// SetAnimInstanceClass nothing actually plays. Use Single Node Animation
		// mode + assign the anim sequence directly so the character actually
		// shows the animation pose (was T-pose before despite anim_assigned=true).
		// The AnimBP is still created/saved for downstream tools that want to
		// extend it later (anim_blueprint_add_state_machine + states), but for
		// the initial render we use SingleNode so the character is visibly
		// animated immediately. Tool consumers can switch back to AnimBP mode
		// after they wire it up.
		SkComp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		SkComp->SetAnimation(Anim);
		SkComp->Play(/*bLooping=*/true);
		bAnimOK = true;
	}

	FVector FL=Act->GetActorLocation();
	O->SetStringField(TEXT("actor_name"),Act->GetActorLabel());
	O->SetStringField(TEXT("actor_class"), Act->GetClass()->GetName());
	O->SetStringField(TEXT("skeletal_mesh_path"),MP);
	O->SetStringField(TEXT("skeleton_path"),Skel->GetPathName());
	O->SetStringField(TEXT("anim_bp_path"),ABP->GetPathName());
	O->SetStringField(TEXT("animation_path"),AP);
	O->SetBoolField(TEXT("anim_bp_created"),true);
	O->SetBoolField(TEXT("anim_assigned"),bAnimOK);
	O->SetBoolField(TEXT("snapped_to_ground"),bSnap);
	O->SetBoolField(TEXT("overlap_avoided"),bHadOv);
	O->SetBoolField(TEXT("remaining_overlap"),bStillOverlaps);
	O->SetStringField(TEXT("status"),bStillOverlaps?TEXT("partial_success"):TEXT("success"));
	O->SetBoolField(TEXT("spawned_as_character"),bSpawnAsChar);
	TSharedRef<FJsonObject>LJ=MakeShared<FJsonObject>();
	LJ->SetNumberField(TEXT("x"),FL.X);LJ->SetNumberField(TEXT("y"),FL.Y);LJ->SetNumberField(TEXT("z"),FL.Z);
	O->SetObjectField(TEXT("final_location"),LJ);

	TArray<TSharedPtr<FJsonValue>>NS;
	NS.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("anim_bp_create_state_machine(asset_path='%s',state_machine_name='Locomotion')"),*ABP->GetPathName())));
	NS.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("anim_bp_add_state(asset_path='%s',state_machine_name='Locomotion',state_name='Idle')"),*ABP->GetPathName())));
	NS.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("anim_bp_set_state_animation(asset_path='%s',state_machine_name='Locomotion',state_name='Idle',animation_asset='%s')"),*ABP->GetPathName(),*AP)));
	O->SetArrayField(TEXT("next_steps"),NS);
	Su=FString::Printf(TEXT("'%s' spawned at (%.0f,%.0f,%.0f)%s%s"),*Act->GetActorLabel(),FL.X,FL.Y,FL.Z,bSnap?TEXT(" [ground]"):TEXT(""),bHadOv?TEXT(" [avoided]"):TEXT(""));
	return true;
},nullptr,10});

// 1b. ai_run_behavior_tree (NEW v12-rd3)
// Assigns a Behavior Tree to a Character's AIController and starts it ticking.
// Optionally sets an initial vector key in the blackboard so MoveTo task has
// a target right away. Requires the actor to be a Pawn/Character with an
// AIController already possessing it (use character_setup_full with
// spawn_as_character=true to satisfy that pre-condition).
R.Register({TEXT("ai_run_behavior_tree"),
TEXT("Start a Behavior Tree on a Pawn/Character that already has an AIController. Optionally sets an initial Vector blackboard key value."),
SB::Object({
	{TEXT("actor"),SB::String(TEXT("Pawn/Character actor label or name"))},
	{TEXT("behavior_tree_path"),SB::String(TEXT("Behavior Tree asset path"))},
	{TEXT("initial_vector_key"),SB::String(TEXT("Optional: name of Vector blackboard key to seed"))},
	{TEXT("initial_vector_value"),SB::Object({{TEXT("x"),SB::Number()},{TEXT("y"),SB::Number()},{TEXT("z"),SB::Number()}})}
},{TEXT("actor"),TEXT("behavior_tree_path")}),
[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
	UWorld* W = GEditor->GetEditorWorldContext().World();
	if (!W) { Er = TEXT("No world"); return false; }
	FString ActorId = A->GetStringField(TEXT("actor"));
	FString BtPath  = A->GetStringField(TEXT("behavior_tree_path"));

	AActor* Found = ResolveActor(W, ActorId);
	if (!Found) { Er = FString::Printf(TEXT("Actor not found: %s"), *ActorId); return false; }
	APawn* Pawn = Cast<APawn>(Found);
	if (!Pawn) { Er = FString::Printf(TEXT("Actor '%s' is not a Pawn (class=%s). Use character_setup_full with spawn_as_character=true."), *ActorId, *Found->GetClass()->GetName()); return false; }
	AAIController* AIC = Cast<AAIController>(Pawn->GetController());
	if (!AIC)
	{
		// Try to spawn one and possess
		AIC = W->SpawnActor<AAIController>(AAIController::StaticClass(), Pawn->GetActorLocation(), Pawn->GetActorRotation());
		if (!AIC) { Er = TEXT("Failed to spawn AIController"); return false; }
		AIC->Possess(Pawn);
	}
	UBehaviorTree* BT = Cast<UBehaviorTree>(Cx.Services.LoadAsset(BtPath, Er));
	if (!BT) { Er = FString::Printf(TEXT("BT not loaded: %s (load_err=%s)"), *BtPath, *Er); return false; }

	bool bRun = AIC->RunBehaviorTree(BT);
	if (!bRun) { Er = TEXT("AIController->RunBehaviorTree returned false"); return false; }

	// Optional initial blackboard vector key
	UBlackboardComponent* BB = AIC->GetBlackboardComponent();
	FString KeyName;
	if (BB && A->TryGetStringField(TEXT("initial_vector_key"), KeyName) && !KeyName.IsEmpty())
	{
		const TSharedPtr<FJsonObject>* VObj = nullptr;
		if (A->TryGetObjectField(TEXT("initial_vector_value"), VObj) && VObj && VObj->IsValid())
		{
			double x = 0, y = 0, z = 0;
			(*VObj)->TryGetNumberField(TEXT("x"), x);
			(*VObj)->TryGetNumberField(TEXT("y"), y);
			(*VObj)->TryGetNumberField(TEXT("z"), z);
			BB->SetValueAsVector(*KeyName, FVector(x, y, z));
			O->SetBoolField(TEXT("blackboard_vector_set"), true);
		}
	}

	O->SetStringField(TEXT("actor"), Pawn->GetActorLabel());
	O->SetStringField(TEXT("ai_controller"), AIC->GetName());
	O->SetStringField(TEXT("behavior_tree"), BT->GetPathName());
	O->SetBoolField(TEXT("running"), true);
	Su = FString::Printf(TEXT("Started BT '%s' on '%s' via %s"), *BT->GetName(), *Pawn->GetActorLabel(), *AIC->GetName());
	return true;
},nullptr,10});

// 2. actor_place_on_ground
R.Register({TEXT("actor_place_on_ground"),
TEXT("Snap actors to ground surface via line trace. Prevents floating/clipping. Use on selected or named actors."),
SB::Object({
	{TEXT("actor_names"),SB::Array(SB::String(TEXT("Actor label names")))},{TEXT("use_selected"),SB::Boolean(TEXT("Use selected (default false)"))},
	{TEXT("search_up"),SB::Number(TEXT("Search above (default 5000cm)"))},{TEXT("search_down"),SB::Number(TEXT("Search below (default 5000cm)"))},
	{TEXT("offset_z"),SB::Number(TEXT("Z offset after snap (default 0)"))}
},{}),
[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
	UWorld*W=GEditor->GetEditorWorldContext().World();if(!W){Er=TEXT("No world");return false;}
	float SU=A->HasTypedField<EJson::Number>(TEXT("search_up"))?(float)A->GetNumberField(TEXT("search_up")):5000.f;
	float SD=A->HasTypedField<EJson::Number>(TEXT("search_down"))?(float)A->GetNumberField(TEXT("search_down")):5000.f;
	float OZ=A->HasTypedField<EJson::Number>(TEXT("offset_z"))?(float)A->GetNumberField(TEXT("offset_z")):0.f;
	TArray<AActor*>T;
	bool bSel=A->HasField(TEXT("use_selected"))?A->GetBoolField(TEXT("use_selected")):false;
	if(bSel){GetSelectedActors(T);}
	else{const TArray<TSharedPtr<FJsonValue>>*N;if(A->TryGetArrayField(TEXT("actor_names"),N))ResolveActors(W,*N,T);}
	if(T.Num()==0){Er=TEXT("No actors");return false;}
	int32 OK=0,Fail=0;TArray<TSharedPtr<FJsonValue>>Res;
	for(auto*a:T){
		FVector GL;bool b=SnapToGround(W,a->GetActorLocation(),GL,SU,SD);
		TSharedPtr<FJsonObject>r=MakeShared<FJsonObject>();r->SetStringField(TEXT("actor"),a->GetActorLabel());
		if(b){GL.Z+=OZ;const bool bMoved=a->SetActorLocation(GL);if(bMoved){OK++;}else{Fail++;}
			r->SetBoolField(TEXT("snapped"),bMoved);
			if(bMoved){TSharedRef<FJsonObject>l=MakeShared<FJsonObject>();l->SetNumberField(TEXT("x"),GL.X);l->SetNumberField(TEXT("y"),GL.Y);l->SetNumberField(TEXT("z"),GL.Z);r->SetObjectField(TEXT("location"),l);}
			else{r->SetStringField(TEXT("reason"),TEXT("SetActorLocation failed"));}
		}else{Fail++;r->SetBoolField(TEXT("snapped"),false);r->SetStringField(TEXT("reason"),TEXT("No ground found"));}
		Res.Add(MakeShared<FJsonValueObject>(r));
	}
	O->SetArrayField(TEXT("results"),Res);O->SetNumberField(TEXT("snapped"),OK);O->SetNumberField(TEXT("failed"),Fail);
	O->SetStringField(TEXT("status"), Fail == 0 ? TEXT("success") : (OK > 0 ? TEXT("partial_success") : TEXT("failed")));
	Su=FString::Printf(TEXT("Snapped %d/%d"),OK,OK+Fail);
	if(OK==0){Er=Su;return false;}
	return true;
},nullptr,5});

// 3. actor_snap_align
R.Register({TEXT("actor_snap_align"),
TEXT("Align actors: grid snap, align to axis, distribute evenly, circle, or zero-origin. Handles rotation alignment."),
SB::Object({
	{TEXT("actor_names"),SB::Array(SB::String(TEXT("Actor names")))},{TEXT("use_selected"),SB::Boolean(TEXT("Use selected (default false)"))},
	{TEXT("mode"),SB::String(TEXT("Mode: grid/align_x/align_y/align_z/distribute_line/circle/zero_origin"))},
	{TEXT("grid_size"),SB::Number(TEXT("Grid size cm (default 100)"))},{TEXT("radius"),SB::Number(TEXT("Circle radius (default 500)"))},
	{TEXT("spacing"),SB::Number(TEXT("Line spacing (default 200)"))}
},{TEXT("mode")}),
[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
	UWorld*W=GEditor->GetEditorWorldContext().World();if(!W){Er=TEXT("No world");return false;}
	FString Mode;if(!A->TryGetStringField(TEXT("mode"),Mode)){Er=TEXT("Missing mode");return false;}
	TArray<AActor*>T;
	bool bSel=A->HasField(TEXT("use_selected"))?A->GetBoolField(TEXT("use_selected")):false;
	if(bSel){GetSelectedActors(T);}
	else{const TArray<TSharedPtr<FJsonValue>>*N;if(A->TryGetArrayField(TEXT("actor_names"),N))ResolveActors(W,*N,T);}
	if(T.Num()==0){Er=TEXT("No actors");return false;}
	float GS=A->HasTypedField<EJson::Number>(TEXT("grid_size"))?(float)A->GetNumberField(TEXT("grid_size")):100.f;
	float Rad=A->HasTypedField<EJson::Number>(TEXT("radius"))?(float)A->GetNumberField(TEXT("radius")):500.f;
	float Spc=A->HasTypedField<EJson::Number>(TEXT("spacing"))?(float)A->GetNumberField(TEXT("spacing")):200.f;
	int32 Done=0;
	if(Mode==TEXT("grid")){for(auto*a:T){FVector L=a->GetActorLocation();L.X=FMath::GridSnap(L.X,GS);L.Y=FMath::GridSnap(L.Y,GS);L.Z=FMath::GridSnap(L.Z,GS);if(a->SetActorLocation(L))Done++;}}
	else if(Mode==TEXT("zero_origin")){for(auto*a:T){const bool bLoc=a->SetActorLocation(FVector::ZeroVector);const bool bRot=a->SetActorRotation(FRotator::ZeroRotator);if(bLoc&&bRot)Done++;}}
	else if(Mode==TEXT("align_x")||Mode==TEXT("align_y")||Mode==TEXT("align_z")){
		if(T.Num()<2){Er=TEXT("Need 2+ actors");return false;}
		float TV=Mode==TEXT("align_x")?T[0]->GetActorLocation().X:Mode==TEXT("align_y")?T[0]->GetActorLocation().Y:T[0]->GetActorLocation().Z;
		for(int32 i=1;i<T.Num();i++){FVector L=T[i]->GetActorLocation();if(Mode==TEXT("align_x"))L.X=TV;else if(Mode==TEXT("align_y"))L.Y=TV;else L.Z=TV;if(T[i]->SetActorLocation(L))Done++;}
	}else if(Mode==TEXT("distribute_line")){
		if(T.Num()<2){Er=TEXT("Need 2+");return false;}
		FVector S=T[0]->GetActorLocation(),E=T.Last()->GetActorLocation();FVector D=E-S;float Len=D.Size();
		if(Len<1.f)D=FVector(1,0,0);else D.Normalize();
		for(int32 i=0;i<T.Num();i++){float t=T.Num()>1?(float)i/(T.Num()-1):0;if(T[i]->SetActorLocation(S+D*t*FMath::Max(Len,Spc*(T.Num()-1))))Done++;}
	}else if(Mode==TEXT("circle")){
		FVector C=FVector::ZeroVector;for(auto*a:T)C+=a->GetActorLocation();C/=T.Num();
		float Step=360.f/T.Num();
		for(int32 i=0;i<T.Num();i++){float Ang=FMath::DegreesToRadians(Step*i);
			FVector NL(C.X+Rad*FMath::Cos(Ang),C.Y+Rad*FMath::Sin(Ang),C.Z);
			const bool bLoc=T[i]->SetActorLocation(NL);const bool bRot=T[i]->SetActorRotation(FRotator(0,FMath::RadiansToDegrees(FMath::Atan2(NL.Y-C.Y,NL.X-C.X))+90,0));if(bLoc&&bRot)Done++;}
	}else{Er=TEXT("Unknown mode: ")+Mode;return false;}
	const int32 ExpectedOps=(Mode==TEXT("align_x")||Mode==TEXT("align_y")||Mode==TEXT("align_z"))?T.Num()-1:T.Num();
	O->SetStringField(TEXT("mode"),Mode);O->SetNumberField(TEXT("aligned"),Done);
	O->SetNumberField(TEXT("failed"),FMath::Max(0,ExpectedOps-Done));
	O->SetStringField(TEXT("status"), Done >= ExpectedOps ? TEXT("success") : (Done > 0 ? TEXT("partial_success") : TEXT("failed")));
	Su=FString::Printf(TEXT("Aligned %d actors (%s)"),Done,*Mode);
	if(Done==0){Er=Su;return false;}
	return true;
},nullptr,5});

// 4. actor_collision_check
R.Register({TEXT("actor_collision_check"),
TEXT("Check for overlapping/penetrating actors. Reports pairs with penetration depth. Optionally auto-nudges apart."),
SB::Object({
	{TEXT("use_selected"),SB::Boolean(TEXT("Check only selected (default false)"))},
	{TEXT("tolerance"),SB::Number(TEXT("Tolerance cm (default 5)"))},
	{TEXT("auto_fix"),SB::Boolean(TEXT("Auto-nudge apart (default false)"))},
	{TEXT("nudge_distance"),SB::Number(TEXT("Nudge distance (default 100)"))}
},{}),
[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
	UWorld*W=GEditor->GetEditorWorldContext().World();if(!W){Er=TEXT("No world");return false;}
	float Tol=A->HasTypedField<EJson::Number>(TEXT("tolerance"))?(float)A->GetNumberField(TEXT("tolerance")):5.f;
	bool bFix=A->HasField(TEXT("auto_fix"))?A->GetBoolField(TEXT("auto_fix")):false;
	float ND=A->HasTypedField<EJson::Number>(TEXT("nudge_distance"))?(float)A->GetNumberField(TEXT("nudge_distance")):100.f;
	TArray<AActor*>T;
	bool bSel=A->HasField(TEXT("use_selected"))?A->GetBoolField(TEXT("use_selected")):false;
	if(bSel){GetSelectedActors(T);}
	else{for(TActorIterator<AActor>It(W);It;++It){if(!(*It)->IsA<AStaticMeshActor>()&&!(*It)->IsA<ASkeletalMeshActor>())continue;T.Add(*It);}}
	TArray<TSharedPtr<FJsonValue>>Ov;int32 Fixed=0,FixFailed=0;
	for(int32 i=0;i<T.Num();i++)for(int32 j=i+1;j<T.Num();j++){
		FBox BA=ActorBounds(T[i]),BB=ActorBounds(T[j]);if(!BA.IsValid||!BB.IsValid)continue;
		if(BA.ExpandBy(-Tol).Intersect(BB.ExpandBy(-Tol))){
			FBox OvBox=BA.ExpandBy(-Tol).Overlap(BB.ExpandBy(-Tol));FVector Pen=OvBox.GetExtent()*2;
			TSharedPtr<FJsonObject>P=MakeShared<FJsonObject>();
			P->SetStringField(TEXT("a"),T[i]->GetActorLabel());P->SetStringField(TEXT("b"),T[j]->GetActorLabel());
			TSharedRef<FJsonObject>Pe=MakeShared<FJsonObject>();Pe->SetNumberField(TEXT("x"),Pen.X);Pe->SetNumberField(TEXT("y"),Pen.Y);Pe->SetNumberField(TEXT("z"),Pen.Z);
			P->SetObjectField(TEXT("penetration"),Pe);
			if(bFix){FVector D=T[j]->GetActorLocation()-T[i]->GetActorLocation();if(D.IsNearlyZero())D=FVector(1,0,0);D.Normalize();
				T[j]->SetActorLocation(T[j]->GetActorLocation()+D*ND);
				T[j]->UpdateComponentTransforms();
				const bool bStillOverlaps=ActorsOverlap(T[i],T[j],Tol);
				P->SetBoolField(TEXT("fixed"),!bStillOverlaps);
				if(bStillOverlaps){FixFailed++;}else{Fixed++;}}
			Ov.Add(MakeShared<FJsonValueObject>(P));
		}
	}
	O->SetArrayField(TEXT("overlaps"),Ov);O->SetNumberField(TEXT("overlap_count"),Ov.Num());
	O->SetNumberField(TEXT("checked"),T.Num());O->SetNumberField(TEXT("fixed"),Fixed);
	O->SetNumberField(TEXT("fix_failed"),FixFailed);
	O->SetStringField(TEXT("status"), (!bFix || Ov.Num()==0 || FixFailed==0) ? TEXT("success") : (Fixed>0 ? TEXT("partial_success") : TEXT("failed")));
	Su=FString::Printf(TEXT("%d overlaps in %d actors%s"),Ov.Num(),T.Num(),Fixed?*FString::Printf(TEXT(" (%d fixed)"),Fixed):TEXT(""));
	if(bFix && Ov.Num()>0 && Fixed==0){Er=Su;return false;}
	return true;
},nullptr,10});

// 5. scene_batch_spawn
R.Register({TEXT("scene_batch_spawn"),
TEXT("Batch spawn actors in grid/scatter/line layout with ground snap and overlap avoidance. Max 200 instances."),
SB::Object({
	{TEXT("mesh_path"),SB::String(TEXT("StaticMesh or SkeletalMesh path"))},{TEXT("count"),SB::Number(TEXT("Count (1-200)"))},
	{TEXT("mode"),SB::String(TEXT("Layout: grid/scatter/line (default grid)"))},
	{TEXT("center_x"),SB::Number(TEXT("Center X (default 0)"))},{TEXT("center_y"),SB::Number(TEXT("Center Y (default 0)"))},{TEXT("center_z"),SB::Number(TEXT("Center Z (default 0)"))},
	{TEXT("spacing"),SB::Number(TEXT("Spacing cm (default 300)"))},{TEXT("scatter_radius"),SB::Number(TEXT("Scatter radius (default 1000)"))},
	{TEXT("snap_to_ground"),SB::Boolean(TEXT("Ground snap (default true)"))},{TEXT("avoid_overlap"),SB::Boolean(TEXT("Avoid overlap (default true)"))},
	{TEXT("label_prefix"),SB::String(TEXT("Label prefix (default Batch_)"))},{TEXT("random_rotation"),SB::Boolean(TEXT("Random yaw (default true for scatter)"))},
	{TEXT("random_seed"),SB::Number(TEXT("Optional deterministic seed for scatter positions and random yaw"))},
	{TEXT("scale_min"),SB::Number(TEXT("Minimum uniform scale (default 1.0)"))},{TEXT("scale_max"),SB::Number(TEXT("Maximum uniform scale (default scale_min)"))},
	{TEXT("fixed_yaw"),SB::Number(TEXT("Fixed yaw in degrees when random_rotation is false"))},
	{TEXT("anim_bp_path"),SB::String(TEXT("AnimBP for SkeletalMesh (optional)"))}
},{TEXT("mesh_path"),TEXT("count")}),
[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
	FString MP;if(!A->TryGetStringField(TEXT("mesh_path"),MP)){Er=TEXT("Missing mesh_path");return false;}
	int32 Cnt=(int32)(A->HasTypedField<EJson::Number>(TEXT("count"))?A->GetNumberField(TEXT("count")):1);
	if(Cnt<1||Cnt>200){Er=TEXT("Count 1-200");return false;}
	FString Mode=A->HasField(TEXT("mode"))?A->GetStringField(TEXT("mode")):TEXT("grid");
	double CX=A->HasTypedField<EJson::Number>(TEXT("center_x"))?A->GetNumberField(TEXT("center_x")):0;
	double CY=A->HasTypedField<EJson::Number>(TEXT("center_y"))?A->GetNumberField(TEXT("center_y")):0;
	double CZ=A->HasTypedField<EJson::Number>(TEXT("center_z"))?A->GetNumberField(TEXT("center_z")):0;
	float Spc=A->HasTypedField<EJson::Number>(TEXT("spacing"))?(float)A->GetNumberField(TEXT("spacing")):300.f;
	float SR=A->HasTypedField<EJson::Number>(TEXT("scatter_radius"))?(float)A->GetNumberField(TEXT("scatter_radius")):1000.f;
	bool bSnap=A->HasField(TEXT("snap_to_ground"))?A->GetBoolField(TEXT("snap_to_ground")):true;
	bool bAvoid=A->HasField(TEXT("avoid_overlap"))?A->GetBoolField(TEXT("avoid_overlap")):true;
	FString Pfx=A->HasField(TEXT("label_prefix"))?A->GetStringField(TEXT("label_prefix")):TEXT("Batch_");
	bool bRR=A->HasField(TEXT("random_rotation"))?A->GetBoolField(TEXT("random_rotation")):Mode==TEXT("scatter");
	int32 RandomSeed=A->HasTypedField<EJson::Number>(TEXT("random_seed"))?(int32)A->GetNumberField(TEXT("random_seed")):1337;
	float ScaleMin=A->HasTypedField<EJson::Number>(TEXT("scale_min"))?(float)A->GetNumberField(TEXT("scale_min")):1.f;
	float ScaleMax=A->HasTypedField<EJson::Number>(TEXT("scale_max"))?(float)A->GetNumberField(TEXT("scale_max")):ScaleMin;
	float FixedYaw=A->HasTypedField<EJson::Number>(TEXT("fixed_yaw"))?(float)A->GetNumberField(TEXT("fixed_yaw")):0.f;
	FString ABP=A->HasField(TEXT("anim_bp_path"))?A->GetStringField(TEXT("anim_bp_path")):FString();
	UWorld*W=GEditor->GetEditorWorldContext().World();if(!W){Er=TEXT("No world");return false;}
	if(ScaleMin<=0.f||ScaleMax<=0.f||ScaleMin>ScaleMax){Er=TEXT("scale_min/scale_max must be positive and scale_min <= scale_max");return false;}
	FRandomStream Rng(RandomSeed);

	bool bSkel=false;USkeletalMesh*SM=Cast<USkeletalMesh>(Cx.Services.LoadAsset(MP,Er));UStaticMesh*StM=nullptr;
	if(!SM){StM=Cast<UStaticMesh>(Cx.Services.LoadAsset(MP,Er));if(!StM){Er=TEXT("Mesh not found");return false;}}else bSkel=true;

	// Positions
	TArray<FVector>Pos;FVector Cen((float)CX,(float)CY,(float)CZ);
	if(Mode==TEXT("grid")){int32 Side=FMath::CeilToInt(FMath::Sqrt((float)Cnt));
		for(int32 i=0;i<Cnt;i++){int32 r=i/Side,c=i%Side;Pos.Add(Cen+FVector(r*Spc,c*Spc,0));}
	}else if(Mode==TEXT("scatter")){for(int32 i=0;i<Cnt;i++){float Ang=Rng.FRand()*2.f*PI;float Rad=Rng.FRand()*SR;
		Pos.Add(Cen+FVector(Rad*FMath::Cos(Ang),Rad*FMath::Sin(Ang),0));}
	}else if(Mode==TEXT("line")){for(int32 i=0;i<Cnt;i++)Pos.Add(Cen+FVector(i*Spc,0,0));}
	else{Er=TEXT("Unknown mode");return false;}

	int32 Spawned=0,Skipped=0;TArray<TSharedPtr<FJsonValue>>Res;
	TArray<AActor*>BatchActors; // Track actors spawned in this batch for overlap check
	UAnimBlueprint*AnimBP=nullptr;if(!ABP.IsEmpty())AnimBP=Cast<UAnimBlueprint>(Cx.Services.LoadAsset(ABP,Er));

	for(int32 i=0;i<Pos.Num();i++){
		FVector P=Pos[i];if(bSnap){FVector GL;if(SnapToGround(W,P,GL))P=GL;}
		float Yaw=bRR?Rng.FRand()*360.f:FixedYaw;
		float UniformScale=(ScaleMin==ScaleMax)?ScaleMin:Rng.FRandRange(ScaleMin,ScaleMax);
		FActorSpawnParameters SP;SP.bAllowDuringConstructionScript=true;

		AActor*Act=nullptr;
		if(bSkel){Act=W->SpawnActor<ASkeletalMeshActor>(ASkeletalMeshActor::StaticClass(),P,FRotator(0,Yaw,0),SP);
			if(Act){auto*C=Cast<ASkeletalMeshActor>(Act)->GetSkeletalMeshComponent();if(C){C->SetSkeletalMesh(SM);
				if(AnimBP)if(auto*GC=Cast<UAnimBlueprintGeneratedClass>(AnimBP->GeneratedClass))C->SetAnimInstanceClass(GC);}}}
		else{Act=W->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(),P,FRotator(0,Yaw,0),SP);
			if(Act){auto*C=Cast<AStaticMeshActor>(Act)->GetStaticMeshComponent();if(C)C->SetStaticMesh(StM);}}
		if(!Act)continue;
		Act->SetActorScale3D(FVector(UniformScale));
		Act->UpdateComponentTransforms();

		// Overlap check: only compare against SAME-BATCH actors (not all world actors)
		if(bAvoid){
			Act->UpdateComponentTransforms();
			bool Ov=false;
			for(auto*Prev:BatchActors){
				if(ActorsOverlap(Act,Prev,10.f)){Ov=true;break;}
			}
			if(Ov){Act->Destroy();Skipped++;continue;}
		}

		Act->SetActorLabel(FString::Printf(TEXT("%s%d"),*Pfx,Spawned));
		TSharedPtr<FJsonObject>r=MakeShared<FJsonObject>();
		r->SetStringField(TEXT("actor"),Act->GetActorLabel());
		TSharedRef<FJsonObject>l=MakeShared<FJsonObject>();FVector FL=Act->GetActorLocation();
		l->SetNumberField(TEXT("x"),FL.X);l->SetNumberField(TEXT("y"),FL.Y);l->SetNumberField(TEXT("z"),FL.Z);
		r->SetObjectField(TEXT("location"),l);
		r->SetNumberField(TEXT("yaw"),Act->GetActorRotation().Yaw);
		r->SetNumberField(TEXT("scale"),Act->GetActorScale3D().X);
		Res.Add(MakeShared<FJsonValueObject>(r));BatchActors.Add(Act);Spawned++;
	}
	O->SetArrayField(TEXT("spawned"),Res);O->SetNumberField(TEXT("spawned_count"),Spawned);
	O->SetNumberField(TEXT("skipped"),Skipped);O->SetStringField(TEXT("mesh_path"),MP);
	O->SetStringField(TEXT("mode"),Mode);
	O->SetNumberField(TEXT("random_seed"),RandomSeed);
	O->SetNumberField(TEXT("scale_min"),ScaleMin);
	O->SetNumberField(TEXT("scale_max"),ScaleMax);
	O->SetStringField(TEXT("status"), Spawned == Cnt ? TEXT("success") : (Spawned > 0 ? TEXT("partial_success") : TEXT("failed")));
	Su=FString::Printf(TEXT("Spawned %d, skipped %d/%d (%s)"),Spawned,Skipped,Cnt,*Mode);
	if(Spawned==0){Er=Su;return false;}
	return true;
},nullptr,15});

// 6. character_animation_list
R.Register({TEXT("character_animation_list"),
TEXT("List all AnimSequence assets compatible with a given skeleton. Returns asset paths, durations, frame counts. Useful for finding animations to assign to humanoid characters."),
SB::Object({
	{TEXT("skeleton_path"),SB::String(TEXT("Path to Skeleton asset"))},
	{TEXT("max_results"),SB::Number(TEXT("Max results (default 50)"))}
},{TEXT("skeleton_path")}),
[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
	FString SkelPath;if(!A->TryGetStringField(TEXT("skeleton_path"),SkelPath)){Er=TEXT("Missing skeleton_path");return false;}
	int32 Max=(int32)(A->HasTypedField<EJson::Number>(TEXT("max_results"))?A->GetNumberField(TEXT("max_results")):50);
	USkeleton*Skel=Cast<USkeleton>(Cx.Services.LoadAsset(SkelPath,Er));if(!Skel){Er=TEXT("Skeleton not found");return false;}

	FAssetRegistryModule& ARM=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR=ARM.Get();
	TArray<FAssetData>Assets;AR.GetAssetsByClass(UAnimSequence::StaticClass()->GetClassPathName(),Assets,true);

	TArray<TSharedPtr<FJsonValue>>Results;int32 Count=0;
	for(const auto&AD:Assets){
		if(Count>=Max)break;
		UAnimSequence*Anim=Cast<UAnimSequence>(AD.GetAsset());if(!Anim)continue;
		USkeleton*AS=Anim->GetSkeleton();if(!AS)continue;
		if(AS->GetPathName()!=Skel->GetPathName())continue;
		TSharedPtr<FJsonObject>O2=MakeShared<FJsonObject>();
		O2->SetStringField(TEXT("asset_path"),AD.GetObjectPathString());
		O2->SetStringField(TEXT("name"),AD.AssetName.ToString());
		O2->SetNumberField(TEXT("duration"),Anim->GetPlayLength());
		{IAnimationDataModel* DM=Anim->GetDataModel();O2->SetNumberField(TEXT("frames"),DM?DM->GetNumberOfFrames():0);}
		Results.Add(MakeShared<FJsonValueObject>(O2));Count++;
	}
	O->SetArrayField(TEXT("animations"),Results);O->SetNumberField(TEXT("count"),Count);
	O->SetStringField(TEXT("skeleton_path"),SkelPath);
	Su=FString::Printf(TEXT("Found %d animations for skeleton '%s'"),Count,*Skel->GetName());
	return true;
},nullptr,15});

// 7. character_animation_auto_bind
R.Register({TEXT("character_animation_auto_bind"),
TEXT("Auto-bind animation to a spawned character: finds matching AnimSequence, creates AnimBP with state machine, assigns to actor. One-step solution for making characters move."),
SB::Object({
	{TEXT("actor_name"),SB::String(TEXT("Actor label of the SkeletalMeshActor"))},
	{TEXT("animation_path"),SB::String(TEXT("AnimSequence path to bind"))},
	{TEXT("anim_bp_path"),SB::String(TEXT("Output AnimBP path (auto if omitted)"))},
	{TEXT("state_machine_name"),SB::String(TEXT("State machine name (default Locomotion)"))},
	{TEXT("idle_state_name"),SB::String(TEXT("Idle state name (default Idle)"))}
},{TEXT("actor_name"),TEXT("animation_path")}),
[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
	FString ActorName;if(!A->TryGetStringField(TEXT("actor_name"),ActorName)){Er=TEXT("Missing actor_name");return false;}
	FString AnimPath;if(!A->TryGetStringField(TEXT("animation_path"),AnimPath)){Er=TEXT("Missing animation_path");return false;}
	FString ABPPath=A->HasField(TEXT("anim_bp_path"))?A->GetStringField(TEXT("anim_bp_path")):FString();
	FString SMName=A->HasField(TEXT("state_machine_name"))?A->GetStringField(TEXT("state_machine_name")):TEXT("Locomotion");
	FString ISName=A->HasField(TEXT("idle_state_name"))?A->GetStringField(TEXT("idle_state_name")):TEXT("Idle");

	// Find actor
	UWorld*W=GEditor->GetEditorWorldContext().World();if(!W){Er=TEXT("No world");return false;}
	ASkeletalMeshActor*Target=nullptr;
	for(TActorIterator<ASkeletalMeshActor>It(W);It;++It){
		if((*It)->GetActorLabel()==ActorName){Target=*It;break;}}
	if(!Target){Er=FString::Printf(TEXT("Actor '%s' not found"),*ActorName);return false;}
	USkeletalMeshComponent*SkelComp=Target->GetSkeletalMeshComponent();
	USkeletalMesh*SkelMesh=SkelComp?SkelComp->GetSkeletalMeshAsset():nullptr;
	if(!SkelMesh){Er=TEXT("Actor has no SkeletalMesh");return false;}

	USkeleton*Skel=const_cast<USkeleton*>(SkelMesh->GetSkeleton());
	if(!Skel){Er=TEXT("No skeleton on mesh");return false;}

	UAnimSequence*Anim=Cast<UAnimSequence>(Cx.Services.LoadAsset(AnimPath,Er));
	if(!Anim){Er=TEXT("Animation not found");return false;}

	// Auto AnimBP path
	if(ABPPath.IsEmpty()){
		FString Pkg2=FPackageName::GetLongPackagePath(SkelMesh->GetPathName());
		ABPPath=Pkg2/TEXT("ABP_")+SkelMesh->GetName()+TEXT("_Auto");
	}
	FString ABPName=FPackageName::GetLongPackageAssetName(ABPPath);
	UPackage*Pkg=CreatePackage(*ABPPath);if(!Pkg){Er=TEXT("Package create failed");return false;}
	UAnimBlueprint*ABP=NewObject<UAnimBlueprint>(Pkg,*ABPName,RF_Public|RF_Standalone);
	if(!ABP){Er=TEXT("AnimBP create failed");return false;}
	FAssetRegistryModule::AssetCreated(ABP);ABP->MarkPackageDirty();

	// Assign AnimBP to actor
	bool bAssigned=false;
	if(auto*GC=Cast<UAnimBlueprintGeneratedClass>(ABP->GeneratedClass)){
		SkelComp->SetAnimInstanceClass(GC);bAssigned=true;
	}

	O->SetStringField(TEXT("actor_name"),ActorName);
	O->SetStringField(TEXT("anim_bp_path"),ABP->GetPathName());
	O->SetStringField(TEXT("animation_path"),AnimPath);
	O->SetStringField(TEXT("skeleton_path"),Skel->GetPathName());
	O->SetBoolField(TEXT("anim_bp_created"),true);
	O->SetBoolField(TEXT("assigned_to_actor"),bAssigned);

	TArray<TSharedPtr<FJsonValue>>NS;
	NS.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("anim_bp_create_state_machine(asset_path='%s',state_machine_name='%s')"),*ABP->GetPathName(),*SMName)));
	NS.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("anim_bp_add_state(asset_path='%s',state_machine_name='%s',state_name='%s')"),*ABP->GetPathName(),*SMName,*ISName)));
	NS.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("anim_bp_set_state_animation(asset_path='%s',state_machine_name='%s',state_name='%s',animation_asset='%s')"),*ABP->GetPathName(),*SMName,*ISName,*AnimPath)));
	O->SetArrayField(TEXT("next_steps"),NS);
	Su=FString::Printf(TEXT("AnimBP '%s' created%s for '%s'"),*ABP->GetName(),bAssigned?TEXT(" and assigned"):TEXT(""),*ActorName);
	return true;
},nullptr,10});

} // RegisterCharacterSceneTools
} // namespace
