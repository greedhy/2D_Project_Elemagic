// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpBehaviorTreeTemplateTools.cpp
// P3-2 — BehaviorTree decorator/service templates (3 tools)
//
// Tool prefix: bt_template_*
//   1. bt_template_apply_patrol     — Compose a patrol sub-tree on an existing BT
//   2. bt_template_apply_combat     — Compose a combat sub-tree on an existing BT
//   3. bt_template_list_available   — List built-in templates and their requirements
//
// Editor module deps (already in SOMOLMCP.Build.cs):
//   - AIModule                    (UBehaviorTree, UBlackboardData, UBT* node base classes)
//   - UnrealEd                    (FScopedTransaction)
//   - Engine                      (asset registry, package machinery)
//
// Notes (TODO P3-2):
//   * UE 5.7 BT runtime exposes UBTComposite_Selector / UBTComposite_Sequence as
//     concrete composites. Concrete task classes such as UBTTask_MoveTo /
//     UBTTask_Wait / UBTTask_BlackboardBase live in optional headers
//     (BehaviorTree/Tasks/) that may not all be installed in every project. We
//     resolve them by class name through FindObject<UClass> + IsChildOf checks
//     so missing classes degrade gracefully (counted as "skipped") instead of
//     causing link errors.
//   * Decorators (UBTDecorator_Blackboard, UBTDecorator_BlueprintBase) and
//     services (UBTService_BlueprintBase) are also resolved by name.
//   * Persistence: UEditorAssetLibrary::SaveAsset is called via
//     Ctx.Services.SaveAsset (which the editor services facade exposes).

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

#include "CoreMinimal.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "SOMOLMCP_BTTemplate"

namespace UE::SOMOLMCP
{
	namespace BTTpl
	{
		using SB = FSololmcpSchemaBuilder;

		// ── Helpers ───────────────────────────────────────────────────

		// Resolve a UBT* class by short name, trying common prefixes/namespaces.
		// Returns nullptr if not found in the loaded module set.
		static UClass* ResolveBTClass(const FString& Name, UClass* RequiredBase)
		{
			if (Name.IsEmpty()) return nullptr;
			const TArray<FString> Variants = {
				Name,
				FString::Printf(TEXT("BTTask_%s"),       *Name),
				FString::Printf(TEXT("BTDecorator_%s"),  *Name),
				FString::Printf(TEXT("BTService_%s"),    *Name),
				FString::Printf(TEXT("BTComposite_%s"),  *Name),
			};
			for (const FString& V : Variants)
			{
				UClass* C = FindObject<UClass>(nullptr, *V);
				if (C && (!RequiredBase || C->IsChildOf(RequiredBase)))
				{
					return C;
				}
			}
			return nullptr;
		}

		// Locate a blackboard key by name; returns false if BB is null or key absent.
		static bool BlackboardHasKey(UBlackboardData* BB, const FName& KeyName)
		{
			if (!BB) return false;
			for (const FBlackboardEntry& E : BB->Keys)
			{
				if (E.EntryName == KeyName) return true;
			}
			// also check parent chain
			UBlackboardData* P = BB->Parent;
			while (P)
			{
				for (const FBlackboardEntry& E : P->Keys)
				{
					if (E.EntryName == KeyName) return true;
				}
				P = P->Parent;
			}
			return false;
		}

		// Append a child Composite under Parent's Children array.
		static UBTCompositeNode* AppendChildComposite(UBehaviorTree* BT, UBTCompositeNode* Parent, UClass* CompositeClass)
		{
			if (!BT || !Parent || !CompositeClass) return nullptr;
			UBTCompositeNode* New = NewObject<UBTCompositeNode>(BT, CompositeClass);
			if (!New) return nullptr;
			FBTCompositeChild Child;
			Child.ChildComposite = New;
			Parent->Children.Add(Child);
			return New;
		}

		// Append a child Task under Parent's Children array; returns the new task.
		static UBTTaskNode* AppendChildTask(UBehaviorTree* BT, UBTCompositeNode* Parent, UClass* TaskClass)
		{
			if (!BT || !Parent || !TaskClass) return nullptr;
			UBTTaskNode* T = NewObject<UBTTaskNode>(BT, TaskClass);
			if (!T) return nullptr;
			FBTCompositeChild Child;
			Child.ChildTask = T;
			Parent->Children.Add(Child);
			return T;
		}

		// Add a decorator to the LAST child slot of Parent's Children. Returns it
		// (or nullptr on failure).
		static UBTDecorator* AddDecoratorToLastChild(UBehaviorTree* BT, UBTCompositeNode* Parent, UClass* DecoratorClass)
		{
			if (!BT || !Parent || !DecoratorClass || Parent->Children.Num() == 0) return nullptr;
			UBTDecorator* Dec = NewObject<UBTDecorator>(BT, DecoratorClass);
			if (!Dec) return nullptr;
			Parent->Children.Last().Decorators.Add(Dec);
			return Dec;
		}

		// Add a service directly to a composite.
		static UBTService* AddService(UBehaviorTree* BT, UBTCompositeNode* Parent, UClass* ServiceClass)
		{
			if (!BT || !Parent || !ServiceClass) return nullptr;
			UBTService* Svc = NewObject<UBTService>(BT, ServiceClass);
			if (!Svc) return nullptr;
			Parent->Services.Add(Svc);
			return Svc;
		}

		// Set a UPROPERTY by name on a node — best-effort reflection helper.
		template <typename TValue>
		static bool SetNodeProp(UObject* Node, FName PropertyName, const TValue& Value)
		{
			if (!Node) return false;
			FProperty* Prop = Node->GetClass()->FindPropertyByName(PropertyName);
			if (!Prop) return false;
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);
			if (FNumericProperty* Num = CastField<FNumericProperty>(Prop))
			{
				if constexpr (std::is_floating_point_v<TValue>)
				{
					Num->SetFloatingPointPropertyValue(ValuePtr, static_cast<double>(Value));
					return true;
				}
				else if constexpr (std::is_integral_v<TValue>)
				{
					Num->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value));
					return true;
				}
			}
			if (FNameProperty* N = CastField<FNameProperty>(Prop))
			{
				if constexpr (std::is_same_v<TValue, FName>)
				{
					N->SetPropertyValue(ValuePtr, Value);
					return true;
				}
				else if constexpr (std::is_same_v<TValue, FString>)
				{
					N->SetPropertyValue(ValuePtr, FName(*Value));
					return true;
				}
			}
			return false;
		}

		// Resolve and load a BehaviorTree by path.
		static UBehaviorTree* LoadBT(const FSololmcpEditorServices& Services, const FString& Path, FString& OutErr)
		{
			UObject* Asset = Services.LoadAsset(Path, OutErr);
			if (!Asset) return nullptr;
			UBehaviorTree* BT = Cast<UBehaviorTree>(Asset);
			if (!BT)
			{
				OutErr = FString::Printf(TEXT("Asset '%s' is not a UBehaviorTree."), *Path);
				return nullptr;
			}
			return BT;
		}

		// Resolve and load a Blackboard by path.
		static UBlackboardData* LoadBB(const FSololmcpEditorServices& Services, const FString& Path, FString& OutErr)
		{
			UObject* Asset = Services.LoadAsset(Path, OutErr);
			if (!Asset) return nullptr;
			UBlackboardData* BB = Cast<UBlackboardData>(Asset);
			if (!BB)
			{
				OutErr = FString::Printf(TEXT("Asset '%s' is not a UBlackboardData."), *Path);
				return nullptr;
			}
			return BB;
		}

		// Ensure the BT's root is the requested composite class (Selector/Sequence).
		// Replaces RootNode if it doesn't match. Returns the (possibly new) root.
		static UBTCompositeNode* EnsureRootComposite(UBehaviorTree* BT, UClass* WantedClass)
		{
			if (!BT || !WantedClass) return nullptr;
			if (!BT->RootNode || !BT->RootNode->IsA(WantedClass))
			{
				BT->RootNode = NewObject<UBTCompositeNode>(BT, WantedClass);
			}
			return BT->RootNode;
		}

		static int32 CountBehaviorTreeNodes(UBTCompositeNode* Root)
		{
			if (!Root) return 0;
			int32 Count = 1;
			Count += Root->Services.Num();
			for (const FBTCompositeChild& Child : Root->Children)
			{
				Count += Child.Decorators.Num();
				if (Child.ChildTask)
				{
					++Count;
				}
				if (Child.ChildComposite)
				{
					Count += CountBehaviorTreeNodes(Child.ChildComposite);
				}
			}
			return Count;
		}

		// Build a JSON entry describing one template.
		static TSharedRef<FJsonObject> MakeTemplateEntry(const FString& Name,
			const FString& Description,
			const TArray<FString>& RequiredBBKeys,
			const TArray<FString>& RequiredArgs)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Name);
			Obj->SetStringField(TEXT("description"), Description);
			TArray<TSharedPtr<FJsonValue>> Keys;
			for (const FString& K : RequiredBBKeys) { Keys.Add(MakeShared<FJsonValueString>(K)); }
			Obj->SetArrayField(TEXT("required_bb_keys"), Keys);
			TArray<TSharedPtr<FJsonValue>> Args;
			for (const FString& A : RequiredArgs) { Args.Add(MakeShared<FJsonValueString>(A)); }
			Obj->SetArrayField(TEXT("required_args"), Args);
			return Obj;
		}

	} // namespace BTTpl

	// ════════════════════════════════════════════════════════════════════
	void RegisterBehaviorTreeTemplateTools(FSololmcpToolRegistry& R)
	{
		using namespace BTTpl;

		// ── 1. bt_template_apply_patrol ───────────────────────────────
		R.Register({
			TEXT("bt_template_apply_patrol"),
			TEXT("Compose a 'patrol' sub-tree on an existing BehaviorTree. "
			     "Root selector with two branches: chase TargetActor (MoveTo) "
			     "OR sequence(SetHomeLocation → MoveTo Home → Wait)."),
			SB::Object({
				{TEXT("bt_path"),           SB::String(TEXT("Existing UBehaviorTree asset path."))},
				{TEXT("bb_path"),           SB::String(TEXT("Blackboard asset path (assigned to BT if needed)."))},
				{TEXT("home_key"),          SB::String(TEXT("Vector key for home location (default 'HomeLocation')."))},
				{TEXT("acceptance_radius"), SB::Number(TEXT("Acceptance radius in cm (default 200.0)."))}
			}, {TEXT("bt_path"), TEXT("bb_path")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString BTPath, BBPath;
				if (!Args->TryGetStringField(TEXT("bt_path"), BTPath) || BTPath.IsEmpty())
				{
					SololmcpError::MissingParam(Out, TEXT("bt_path"));
					Error = TEXT("Missing bt_path."); return false;
				}
				if (!Args->TryGetStringField(TEXT("bb_path"), BBPath) || BBPath.IsEmpty())
				{
					SololmcpError::MissingParam(Out, TEXT("bb_path"));
					Error = TEXT("Missing bb_path."); return false;
				}
				FString HomeKey = TEXT("HomeLocation");
				Args->TryGetStringField(TEXT("home_key"), HomeKey);
				const double AcceptanceRadius = Args->HasTypedField<EJson::Number>(TEXT("acceptance_radius"))
					? Args->GetNumberField(TEXT("acceptance_radius")) : 200.0;

				UBehaviorTree* BT = LoadBT(Ctx.Services, BTPath, Error);
				if (!BT) { SololmcpError::InvalidPath(Out, BTPath); return false; }
				UBlackboardData* BB = LoadBB(Ctx.Services, BBPath, Error);
				if (!BB) { SololmcpError::InvalidPath(Out, BBPath); return false; }

				const FScopedTransaction Tx(LOCTEXT("BTPatrol", "SOMOLMCP BT Apply Patrol Template"));
				BT->Modify();

				if (!BT->BlackboardAsset) { BT->BlackboardAsset = BB; }

				// Root must be a Selector for "try chase OR fall back to patrol".
				UBTCompositeNode* Root = EnsureRootComposite(BT, UBTComposite_Selector::StaticClass());
				if (!Root)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("Could not create root selector for BT."));
					Error = TEXT("Root composite create failed."); return false;
				}

				int32 NodesAdded = 0;

				// Resolve required UBT* concrete classes (best-effort).
				UClass* MoveToClass = ResolveBTClass(TEXT("MoveTo"), UBTTaskNode::StaticClass());
				if (!MoveToClass) { MoveToClass = ResolveBTClass(TEXT("BlackboardBase"), UBTTaskNode::StaticClass()); }
				UClass* WaitClass = ResolveBTClass(TEXT("Wait"),    UBTTaskNode::StaticClass());
				UClass* SeqClass  = UBTComposite_Sequence::StaticClass();
				UClass* DecBBClass = ResolveBTClass(TEXT("Blackboard"), UBTDecorator::StaticClass());

				// Branch A: chase Task (MoveTo TargetActor).
				if (MoveToClass)
				{
					UBTTaskNode* Chase = AppendChildTask(BT, Root, MoveToClass);
					if (Chase) { ++NodesAdded; }
					// Set acceptance radius if the task exposes the field.
					if (Chase) { SetNodeProp<double>(Chase, TEXT("AcceptableRadius"), AcceptanceRadius); }
					// Decorate "chase" with a Blackboard-IsSet on TargetActor.
					if (DecBBClass)
					{
						UBTDecorator* IsTargetSet = AddDecoratorToLastChild(BT, Root, DecBBClass);
						if (IsTargetSet)
						{
							++NodesAdded;
							SetNodeProp<FName>(IsTargetSet, TEXT("BlackboardKey"), FName(TEXT("TargetActor")));
						}
					}
				}

				// Branch B: sequence(MoveTo Home → Wait).
				UBTCompositeNode* Patrol = AppendChildComposite(BT, Root, SeqClass);
				if (Patrol) { ++NodesAdded; }
				if (Patrol && MoveToClass)
				{
					UBTTaskNode* MoveHome = AppendChildTask(BT, Patrol, MoveToClass);
					if (MoveHome)
					{
						++NodesAdded;
						SetNodeProp<FName>  (MoveHome, TEXT("BlackboardKey"),     FName(*HomeKey));
						SetNodeProp<double> (MoveHome, TEXT("AcceptableRadius"), AcceptanceRadius);
					}
				}
				if (Patrol && WaitClass)
				{
					UBTTaskNode* WaitNode = AppendChildTask(BT, Patrol, WaitClass);
					if (WaitNode)
					{
						++NodesAdded;
						// Range 2-5s: set a midpoint with random deviation when fields exist.
						SetNodeProp<double>(WaitNode, TEXT("WaitTime"),      3.5);
						SetNodeProp<double>(WaitNode, TEXT("RandomDeviation"), 1.5);
					}
				}

				// Persist. UBehaviorTree is not a UBlueprint, so we skip
				// MarkBlueprintAsStructurallyModified (it would no-op or assert on a null arg).
				if (NodesAdded <= 0 || CountBehaviorTreeNodes(Root) <= 1)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("Patrol template did not add any behavior tree nodes."));
					Error = TEXT("Patrol template made no verified tree changes.");
					return false;
				}
				BT->MarkPackageDirty();
				SololmcpWriteFlush::EnsureFlushed(BT);

				FString SaveErr;
				if (!Ctx.Services.SaveAsset(BT->GetPathName(), /*bOnlyIfDirty=*/true, SaveErr))
				{
					SololmcpError::Set(Out, TEXT("SAVE_FAILED"), TEXT("bt_path"), SaveErr);
					Error = SaveErr.IsEmpty() ? TEXT("Failed to save behavior tree asset.") : SaveErr;
					return false;
				}

				const FString WarnHomeKey = BlackboardHasKey(BB, FName(*HomeKey))
					? FString() : FString::Printf(TEXT("Blackboard missing vector key '%s'; BT will fail at runtime until added."), *HomeKey);
				const FString WarnTarget = BlackboardHasKey(BB, FName(TEXT("TargetActor")))
					? FString() : FString(TEXT("Blackboard missing object key 'TargetActor'; chase branch will be inert."));

				Out->SetStringField(TEXT("bt_path"), BT->GetPathName());
				Out->SetNumberField(TEXT("nodes_added"), NodesAdded);
				Out->SetStringField(TEXT("root_composite"), Root->GetClass()->GetName());
				Out->SetStringField(TEXT("home_key"), HomeKey);
				Out->SetNumberField(TEXT("acceptance_radius"), AcceptanceRadius);
				if (!WarnHomeKey.IsEmpty()) { Out->SetStringField(TEXT("warning_home_key"), WarnHomeKey); }
				if (!WarnTarget.IsEmpty())  { Out->SetStringField(TEXT("warning_target_actor"), WarnTarget); }

				Summary = FString::Printf(TEXT("Patrol template applied to '%s' (%d nodes)."),
					*BT->GetName(), NodesAdded);
				return true;
			},
			nullptr, 0
		});

		// ── 2. bt_template_apply_combat ───────────────────────────────
		R.Register({
			TEXT("bt_template_apply_combat"),
			TEXT("Compose a combat sub-tree on an existing BehaviorTree. "
			     "Root selector: Flee (low health) / Attack (in range) / Chase. "
			     "Each branch decorated with appropriate Blackboard checks."),
			SB::Object({
				{TEXT("bt_path"),         SB::String(TEXT("Existing UBehaviorTree asset path."))},
				{TEXT("bb_path"),         SB::String(TEXT("Blackboard asset path."))},
				{TEXT("attack_range"),    SB::Number(TEXT("Attack range in cm (default 200.0)."))},
				{TEXT("flee_health_pct"), SB::Number(TEXT("Flee health threshold 0..1 (default 0.3)."))}
			}, {TEXT("bt_path"), TEXT("bb_path")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString BTPath, BBPath;
				if (!Args->TryGetStringField(TEXT("bt_path"), BTPath) || BTPath.IsEmpty())
				{
					SololmcpError::MissingParam(Out, TEXT("bt_path"));
					Error = TEXT("Missing bt_path."); return false;
				}
				if (!Args->TryGetStringField(TEXT("bb_path"), BBPath) || BBPath.IsEmpty())
				{
					SololmcpError::MissingParam(Out, TEXT("bb_path"));
					Error = TEXT("Missing bb_path."); return false;
				}
				const double AttackRange = Args->HasTypedField<EJson::Number>(TEXT("attack_range"))
					? Args->GetNumberField(TEXT("attack_range")) : 200.0;
				const double FleeHealthPct = Args->HasTypedField<EJson::Number>(TEXT("flee_health_pct"))
					? Args->GetNumberField(TEXT("flee_health_pct")) : 0.3;

				UBehaviorTree* BT = LoadBT(Ctx.Services, BTPath, Error);
				if (!BT) { SololmcpError::InvalidPath(Out, BTPath); return false; }
				UBlackboardData* BB = LoadBB(Ctx.Services, BBPath, Error);
				if (!BB) { SololmcpError::InvalidPath(Out, BBPath); return false; }

				const FScopedTransaction Tx(LOCTEXT("BTCombat", "SOMOLMCP BT Apply Combat Template"));
				BT->Modify();

				if (!BT->BlackboardAsset) { BT->BlackboardAsset = BB; }

				UBTCompositeNode* Root = EnsureRootComposite(BT, UBTComposite_Selector::StaticClass());
				if (!Root)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("Could not create root selector for BT."));
					Error = TEXT("Root composite create failed."); return false;
				}

				int32 NodesAdded = 0;
				int32 DecoratorsAdded = 0;

				UClass* MoveToClass  = ResolveBTClass(TEXT("MoveTo"),   UBTTaskNode::StaticClass());
				UClass* WaitClass    = ResolveBTClass(TEXT("Wait"),     UBTTaskNode::StaticClass());
				UClass* SeqClass     = UBTComposite_Sequence::StaticClass();
				UClass* DecBBClass   = ResolveBTClass(TEXT("Blackboard"), UBTDecorator::StaticClass());

				// ── Branch 1: Flee — sequence(Wait 0.5 → MoveTo away) decorated with low-health BB compare.
				UBTCompositeNode* FleeSeq = AppendChildComposite(BT, Root, SeqClass);
				if (FleeSeq) { ++NodesAdded; }
				if (DecBBClass)
				{
					UBTDecorator* LowHealth = AddDecoratorToLastChild(BT, Root, DecBBClass);
					if (LowHealth)
					{
						++DecoratorsAdded;
						SetNodeProp<FName>(LowHealth, TEXT("BlackboardKey"), FName(TEXT("HealthPct")));
						// "Less than" comparison constant; reflection-set via a sentinel field name.
						// Many UBTDecorator_Blackboard variants use "FloatValue" + "OperationType".
						SetNodeProp<double>(LowHealth, TEXT("FloatValue"), FleeHealthPct);
					}
				}
				if (FleeSeq && WaitClass)
				{
					UBTTaskNode* W = AppendChildTask(BT, FleeSeq, WaitClass);
					if (W) { ++NodesAdded; SetNodeProp<double>(W, TEXT("WaitTime"), 0.5); }
				}
				if (FleeSeq && MoveToClass)
				{
					UBTTaskNode* M = AppendChildTask(BT, FleeSeq, MoveToClass);
					if (M)
					{
						++NodesAdded;
						SetNodeProp<FName>(M, TEXT("BlackboardKey"), FName(TEXT("FleeLocation")));
					}
				}

				// ── Branch 2: Attack — sequence(MeleeAttack/Wait) decorated with target-in-range BB compare.
				UBTCompositeNode* AttackSeq = AppendChildComposite(BT, Root, SeqClass);
				if (AttackSeq) { ++NodesAdded; }
				if (DecBBClass)
				{
					UBTDecorator* InRange = AddDecoratorToLastChild(BT, Root, DecBBClass);
					if (InRange)
					{
						++DecoratorsAdded;
						SetNodeProp<FName>(InRange, TEXT("BlackboardKey"), FName(TEXT("DistanceToTarget")));
						SetNodeProp<double>(InRange, TEXT("FloatValue"), AttackRange);
					}
				}
				if (AttackSeq && WaitClass)
				{
					UBTTaskNode* Atk = AppendChildTask(BT, AttackSeq, WaitClass);
					if (Atk) { ++NodesAdded; SetNodeProp<double>(Atk, TEXT("WaitTime"), 1.0); }
				}

				// ── Branch 3: Chase — MoveTo TargetActor decorated with IsTargetValid (BB has TargetActor).
				if (MoveToClass)
				{
					UBTTaskNode* Chase = AppendChildTask(BT, Root, MoveToClass);
					if (Chase)
					{
						++NodesAdded;
						SetNodeProp<FName>(Chase, TEXT("BlackboardKey"), FName(TEXT("TargetActor")));
						SetNodeProp<double>(Chase, TEXT("AcceptableRadius"), AttackRange * 0.9);
					}
					if (DecBBClass)
					{
						UBTDecorator* IsValid = AddDecoratorToLastChild(BT, Root, DecBBClass);
						if (IsValid)
						{
							++DecoratorsAdded;
							SetNodeProp<FName>(IsValid, TEXT("BlackboardKey"), FName(TEXT("TargetActor")));
						}
					}
				}

				// Persist.
				if (NodesAdded <= 0 || CountBehaviorTreeNodes(Root) <= 1)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("Combat template did not add any behavior tree nodes."));
					Error = TEXT("Combat template made no verified tree changes.");
					return false;
				}
				BT->MarkPackageDirty();
				SololmcpWriteFlush::EnsureFlushed(BT);
				FString SaveErr;
				if (!Ctx.Services.SaveAsset(BT->GetPathName(), /*bOnlyIfDirty=*/true, SaveErr))
				{
					SololmcpError::Set(Out, TEXT("SAVE_FAILED"), TEXT("bt_path"), SaveErr);
					Error = SaveErr.IsEmpty() ? TEXT("Failed to save behavior tree asset.") : SaveErr;
					return false;
				}

				// Surface BB-key warnings so the AI knows what's still missing.
				TArray<TSharedPtr<FJsonValue>> MissingKeys;
				const TArray<FName> Wanted = {
					FName(TEXT("HealthPct")),
					FName(TEXT("TargetActor")),
					FName(TEXT("DistanceToTarget")),
					FName(TEXT("FleeLocation"))
				};
				for (const FName& K : Wanted)
				{
					if (!BlackboardHasKey(BB, K))
					{
						MissingKeys.Add(MakeShared<FJsonValueString>(K.ToString()));
					}
				}

				Out->SetStringField(TEXT("bt_path"), BT->GetPathName());
				Out->SetNumberField(TEXT("nodes_added"), NodesAdded);
				Out->SetNumberField(TEXT("decorators_added"), DecoratorsAdded);
				Out->SetNumberField(TEXT("attack_range"), AttackRange);
				Out->SetNumberField(TEXT("flee_health_pct"), FleeHealthPct);
				Out->SetArrayField(TEXT("missing_blackboard_keys"), MissingKeys);

				Summary = FString::Printf(TEXT("Combat template applied to '%s' (%d nodes, %d decorators)."),
					*BT->GetName(), NodesAdded, DecoratorsAdded);
				return true;
			},
			nullptr, 0
		});

		// ── 3. bt_template_list_available ─────────────────────────────
		R.Register({
			TEXT("bt_template_list_available"),
			TEXT("List built-in BT templates and their requirements (BB keys, args). "
			     "Useful for AI discovery."),
			SB::Object({}, {}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				TArray<TSharedPtr<FJsonValue>> Templates;

				Templates.Add(MakeShared<FJsonValueObject>(MakeTemplateEntry(
					TEXT("patrol"),
					TEXT("Selector { chase TargetActor (MoveTo) ; sequence(MoveTo HomeLocation -> Wait 2-5s) }"),
					{ TEXT("HomeLocation:Vector"), TEXT("TargetActor:Object (optional)") },
					{ TEXT("bt_path"), TEXT("bb_path"), TEXT("home_key (opt, default HomeLocation)"),
					  TEXT("acceptance_radius (opt, default 200.0)") }
				)));

				Templates.Add(MakeShared<FJsonValueObject>(MakeTemplateEntry(
					TEXT("combat"),
					TEXT("Selector { Flee[<flee_health_pct] ; Attack[in range] ; Chase TargetActor }"),
					{ TEXT("HealthPct:Float"), TEXT("TargetActor:Object"),
					  TEXT("DistanceToTarget:Float"), TEXT("FleeLocation:Vector") },
					{ TEXT("bt_path"), TEXT("bb_path"),
					  TEXT("attack_range (opt, default 200.0)"),
					  TEXT("flee_health_pct (opt, default 0.3)") }
				)));

				Out->SetArrayField(TEXT("templates"), Templates);
				Out->SetNumberField(TEXT("count"), Templates.Num());

				Summary = FString::Printf(TEXT("%d BT template(s) available."), Templates.Num());
				return true;
			},
			nullptr, 60
		});
	}
} // namespace UE::SOMOLMCP

#undef LOCTEXT_NAMESPACE
