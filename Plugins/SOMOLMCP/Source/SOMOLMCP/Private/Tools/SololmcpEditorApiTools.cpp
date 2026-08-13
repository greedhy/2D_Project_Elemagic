// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// Reflection dispatch over large editor APIs.
//
// Twelve curated domains cover the surfaces worth naming -- DynamicMaterial,
// ClonerEffector, USD, Datasmith, Avalanche, Sequencer, Movie Render Queue, UMG,
// PCG, audio and the rest -- and explicit module addressing reaches everything else,
// so the dispatcher spans the engine's entire BlueprintCallable surface.
// Writing tools by hand at that scale is not a plan, and a dispatcher per domain
// would be the same file twelve times over. Adding a domain is one row in the table
// below; the four tools do not change.
//
// Two properties make one dispatcher enough:
//
// First, reflection needs no link dependency. These tools reach classes through
// TObjectIterator, which sees whatever the running editor has registered. That means
// no module in Build.cs, no third-party USD libraries pulled into the plugin, and no
// per-engine-version gating -- the same binary works on 5.3 through 5.8, and a
// domain whose plugin is disabled simply reports itself absent rather than failing
// to build. The GeometryScripting dispatcher needed headers only for its typed
// session tools, not for dispatch itself.
//
// Second, unlike GeometryScripting these APIs are mostly *instance* methods -- 807
// of the four plugin domains' ~1,190, and 766 of Sequencer/MRQ's ~1,060 -- hanging
// off live objects such as a material slot, a cloner extension, a movie graph node
// or a sequence binding rather than off function libraries. Those objects are
// reachable only because calls that return one register it as a session handle, so
// a caller walks the object graph one call at a time. Without handles this surface
// is unreachable no matter how many tools are written for it.
//
// On overlap with existing tools: the sequencer and movie_render domains sit behind
// SOMOLMCP's hand-written cinematic tools rather than replacing them. Those tools
// encode workflow -- correct transaction scoping, batch semantics, receipts -- that
// a generic call cannot infer from a signature. This is the long tail, and callers
// should prefer the named tool where one exists.
//
// Scope, and how the whole surface is reached.
//
// The twelve curated domains are 5,232 of the 20,048 BlueprintCallable functions the
// running editor registers across 274 modules -- 26%. The remainder is reachable by
// naming a module directly: domain="module:<ModuleName>".
//
// That is deliberately not the same thing as a blanket "call any UFUNCTION" tool.
// The module has to be named at the call site, so nothing outside a curated domain
// is ever reached implicitly or through a loose prefix, and the match is exact:
// module:Engine is the Engine module (4,224 functions), not everything starting with
// "Engine". Calls still go through the queue and the target guard, so a write with
// no resolvable target is still blocked fail-closed.
//
// It does widen what is callable, and that is a real change in posture rather than
// an implementation detail: before this, generic invocation of arbitrary engine
// functions was available only through the separate read-only path. Anyone tightening
// this again should do it here, by restricting which modules ResolveDomain will
// synthesise, rather than by removing the curated table.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpObjectHandles.h"
#include "SololmcpReflectionInvoke.h"

#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectIterator.h"

namespace UE::SOMOLMCP
{
namespace EditorApiToolsPrivate
{
	struct FDomain
	{
		const TCHAR* Name = TEXT("");
		const TCHAR* Plugin = TEXT("");
		/// Module-name prefixes that own this domain's classes.
		TArray<FString> ModulePrefixes;
		const TCHAR* Note = TEXT("");
		/// Match the module name exactly instead of by prefix. Set for synthesised
		/// module: domains, where a prefix match would silently widen the scope the
		/// caller asked for.
		bool bExactModule = false;
		/// The name the caller used, for echoing back on a synthesised domain.
		FString SynthesizedName;
	};

	/**
	 * The domains this dispatcher serves.
	 *
	 * Membership is decided by owning module, not by class-name prefix. Class-name
	 * prefixes look adequate and are not: "UDM" also matches the entire DMX plugin
	 * (124 classes on 5.8), which would have quietly put DMX fixture control inside
	 * the dynamic_material domain. A class's package names the module that declared
	 * it, so it answers "does this belong here" exactly rather than by convention.
	 */
	inline const TArray<FDomain>& GetDomains()
	{
		static const TArray<FDomain> Domains = {
			{
				TEXT("dynamic_material"), TEXT("DynamicMaterial"),
				{TEXT("DynamicMaterial")},
				TEXT("Material Designer. Almost entirely instance methods: start from a material "
					 "model asset and walk to slots, layers and stages by handle.")
			},
			{
				TEXT("cloner_effector"), TEXT("ClonerEffector"),
				{TEXT("ClonerEffector")},
				TEXT("Cloner and Effector. Layouts, extensions and effects are components and "
					 "sub-objects reached by handle from a cloner actor.")
			},
			{
				TEXT("usd"), TEXT("USDImporter"),
				{TEXT("USD"), TEXT("UnrealUSDWrapper")},
				TEXT("USD stage and conversion. Mostly static library functions, so most calls "
					 "need no target at all.")
			},
			{
				TEXT("datasmith"), TEXT("Datasmith"),
				{TEXT("Datasmith"), TEXT("Dataprep"), TEXT("VariantManager"),
				 TEXT("LidarPointCloud"), TEXT("DirectLink"), TEXT("GLTFExporter")},
				TEXT("Datasmith import, Dataprep recipes, Variant Manager and Lidar point clouds.")
			},
			{
				TEXT("sequencer"), TEXT("MovieScene / Sequencer"),
				{TEXT("MovieScene"), TEXT("Sequencer"), TEXT("LevelSequence"),
				 TEXT("TemplateSequence"), TEXT("ActorSequence")},
				TEXT("Sequences, tracks, sections, bindings and keys. SOMOLMCP already has hand-written "
					 "tools for the common cinematic operations; this domain is the long tail behind "
					 "them — extensions, scripting-layer key access and per-track configuration.")
			},
			{
				TEXT("movie_render"), TEXT("MovieRenderPipeline"),
				{TEXT("MovieRenderPipeline"), TEXT("MovieGraph")},
				TEXT("Movie Render Queue and the Movie Graph. Largely configuration objects reached "
					 "from a job or a graph node, so most calls need a handle from an earlier one.")
			},
			{
				TEXT("take_recorder"), TEXT("Takes"),
				{TEXT("Take"), TEXT("CacheTrackRecorder")},
				TEXT("Take Recorder: panels, sources, slates and take metadata.")
			},
			{
				TEXT("umg"), TEXT("UMG / CommonUI"),
				{TEXT("UMG"), TEXT("CommonUI"), TEXT("Slate")},
				TEXT("Widgets, CommonUI and the Slate scripting surfaces. A large part of this is "
					 "runtime widget API rather than editor authoring, so prefer the hand-written "
					 "umg_* and slate_* tools for building UI; this reaches the rest.")
			},
			{
				TEXT("anim_rig"), TEXT("IKRig / Mover"),
				{TEXT("IKRig"), TEXT("Mover"), TEXT("AnimGraph")},
				TEXT("IK Rig and retargeting, the Mover movement framework, and AnimGraph runtime "
					 "nodes.")
			},
			{
				TEXT("motion_design"), TEXT("Avalanche"),
				{TEXT("Avalanche")},
				TEXT("Motion Design: modifiers, shapes, transitions, sequences and media.")
			},
			{
				TEXT("audio"), TEXT("Synthesis / MetaSound / AudioMixer"),
				{TEXT("Synthesis"), TEXT("Metasound"), TEXT("AudioMixer"), TEXT("AudioModulation"),
				 TEXT("AudioGameplay"), TEXT("AudioWidgets"), TEXT("AudioInsights"),
				 TEXT("SoundUtilities")},
				TEXT("Synthesis components, MetaSound, the audio mixer, modulation and audio "
					 "gameplay.")
			},
			{
				TEXT("pcg"), TEXT("PCG"),
				{TEXT("PCG")},
				TEXT("Procedural Content Generation graphs, components and interop. SOMOLMCP already "
					 "has substantial hand-written pcg_* coverage; this is the remainder.")
			}
		};
		return Domains;
	}

	/** The module that declared a class, e.g. "DynamicMaterialEditor". */
	inline FString GetOwningModule(const UClass* Class)
	{
		FString Package = Class->GetOutermost()->GetName();
		FString Module;
		return Package.Split(TEXT("/Script/"), nullptr, &Module) ? Module : FString();
	}

	/**
	 * Resolve a domain name.
	 *
	 * The curated domains cover the surfaces worth naming, but they are 12 of ~380
	 * modules and roughly a third of the engine's BlueprintCallable functions. The
	 * rest is reachable as "module:<ModuleName>", which synthesises a domain scoped
	 * to exactly that module.
	 *
	 * Two properties keep this from being a blanket "call anything" facility. The
	 * module must be named explicitly at the call site, so nothing outside a curated
	 * domain is ever reached by accident or by a loose prefix. And the match is exact
	 * rather than prefix-based: "module:Engine" is the Engine module, not everything
	 * beginning with Engine.
	 *
	 * Returned by value because a synthesised domain has no storage in the table.
	 */
	inline bool ResolveDomain(const FString& Name, FDomain& OutDomain, FString& OutError)
	{
		for (const FDomain& Domain : GetDomains())
		{
			if (Name.Equals(Domain.Name, ESearchCase::IgnoreCase))
			{
				OutDomain = Domain;
				return true;
			}
		}

		FString ModuleName;
		if (Name.StartsWith(TEXT("module:"), ESearchCase::IgnoreCase))
		{
			ModuleName = Name.RightChop(7).TrimStartAndEnd();
		}
		if (ModuleName.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("Unknown domain '%s'. Run editor_api_domains for the curated domains, or "
					 "address any module directly as module:<ModuleName>."), *Name);
			return false;
		}

		OutDomain.Name = TEXT("module");
		OutDomain.Plugin = TEXT("(explicit module)");
		OutDomain.ModulePrefixes = { ModuleName };
		OutDomain.bExactModule = true;
		OutDomain.Note = TEXT("Explicitly addressed module.");
		OutDomain.SynthesizedName = Name;
		return true;
	}

	inline bool ClassBelongsTo(const UClass* Class, const FDomain& Domain)
	{
		if (Class->GetName().StartsWith(TEXT("DEPRECATED_")))
		{
			return false;
		}
		const FString Module = GetOwningModule(Class);
		if (Module.IsEmpty())
		{
			// Blueprint-generated classes live in content packages, not /Script/.
			// They are not part of a plugin's declared API surface.
			return false;
		}
		for (const FString& Prefix : Domain.ModulePrefixes)
		{
			const bool bMatch = Domain.bExactModule
				? Module.Equals(Prefix, ESearchCase::IgnoreCase)
				: Module.StartsWith(Prefix, ESearchCase::CaseSensitive);
			if (bMatch)
			{
				return true;
			}
		}
		return false;
	}

	/** Classes of a domain that are actually registered in this editor. */
	inline TArray<UClass*> GetDomainClasses(const FDomain& Domain)
	{
		TArray<UClass*> Result;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}
			if (ClassBelongsTo(Class, Domain))
			{
				Result.Add(Class);
			}
		}
		Result.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
		return Result;
	}

	inline bool IsExposedFunction(const UFunction* Function)
	{
		return Function->HasAnyFunctionFlags(FUNC_BlueprintCallable)
			&& !Function->HasAnyFunctionFlags(FUNC_EditorOnly | FUNC_Delegate)
			&& !Function->HasMetaData(TEXT("DeprecatedFunction"));
	}

	/** The handle kind used for objects a domain hands back. */
	inline FString HandleKindFor(const FDomain& Domain)
	{
		return Domain.SynthesizedName.IsEmpty()
			? FString::Printf(TEXT("%s_object"), Domain.Name)
			: FString::Printf(TEXT("%s_object"), *Domain.SynthesizedName);
	}

	/** What to call this domain in messages: the caller's own name for it. */
	inline FString DomainLabel(const FDomain& Domain)
	{
		return Domain.SynthesizedName.IsEmpty() ? FString(Domain.Name) : Domain.SynthesizedName;
	}

	/**
	 * Resolve a target: a session handle first, then an asset path.
	 *
	 * Handles come first because a handle string could in principle also parse as a
	 * path, and the handle is always the more specific answer -- it names the exact
	 * live object the caller has been working with.
	 */
	inline UObject* ResolveRef(const FString& Ref, UClass* Expected, FString& OutError)
	{
		if (UObject* Handled = FSololmcpObjectHandles::Get().Resolve(Ref))
		{
			if (Expected != nullptr && !Handled->IsA(Expected))
			{
				OutError = FString::Printf(TEXT("Handle '%s' is a %s, but a %s was expected."),
					*Ref, *Handled->GetClass()->GetName(), *Expected->GetName());
				return nullptr;
			}
			return Handled;
		}
		if (!Reflection::IsAssetLikeClass(Expected))
		{
			OutError = FString::Printf(
				TEXT("'%s' names a live scene object; pass a session handle, not a path."),
				Expected != nullptr ? *Expected->GetName() : TEXT("object"));
			return nullptr;
		}
		UObject* Loaded = StaticLoadObject(Expected != nullptr ? Expected : UObject::StaticClass(),
			nullptr, *Ref, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Loaded == nullptr)
		{
			OutError = FString::Printf(
				TEXT("'%s' is neither a live handle nor a loadable %s asset."), *Ref,
				Expected != nullptr ? *Expected->GetName() : TEXT("object"));
		}
		return Loaded;
	}

	/** Locate a function named "Class.Function" or bare, within one domain. */
	inline UFunction* FindDomainFunction(
		const FDomain& Domain, const FString& FunctionName, UClass*& OutClass, bool& bOutAmbiguous)
	{
		OutClass = nullptr;
		bOutAmbiguous = false;

		FString ClassPart;
		FString NamePart;
		if (!FunctionName.Split(TEXT("."), &ClassPart, &NamePart))
		{
			NamePart = FunctionName;
		}

		UFunction* Found = nullptr;
		for (UClass* Class : GetDomainClasses(Domain))
		{
			if (!ClassPart.IsEmpty() && !Class->GetName().Equals(ClassPart, ESearchCase::IgnoreCase))
			{
				continue;
			}
			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				if (!IsExposedFunction(*It) || !It->GetName().Equals(NamePart, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (Found != nullptr)
				{
					bOutAmbiguous = true;
					OutClass = nullptr;
					return nullptr;
				}
				Found = *It;
				OutClass = Class;
			}
		}
		return Found;
	}

	inline Reflection::FInvokePolicy MakePolicy(const FDomain& Domain, const FString& Origin)
	{
		const FString Kind = HandleKindFor(Domain);
		Reflection::FInvokePolicy Policy;
		// Every call registers what it returns, so a component or other live object is
		// nameable as soon as some earlier call has handed it back.
		Policy.bHandlesLiveObjects = true;
		Policy.ResolveObject = [](const FString& Ref, UClass* Expected, FString& OutError) -> UObject*
		{
			return ResolveRef(Ref, Expected, OutError);
		};
		Policy.MintHandle = [Kind, Origin](UObject* Returned) -> FString
		{
			// Registering returned objects is what makes the instance-method majority
			// reachable at all: the next call names this handle as its target. Add()
			// returns the existing handle when the object is already registered, so a
			// repeated walk does not accumulate duplicates -- but each distinct object
			// stays pinned until released, which is why handle_release matters here.
			return Returned != nullptr
				? FSololmcpObjectHandles::Get().Add(Returned, Kind, Origin)
				: FString();
		};
		return Policy;
	}

	/**
	 * Build a receipt for a batch.
	 *
	 * The queue already returns a receipt envelope for a job, but it describes the
	 * job, not what a generic call actually touched. A caller replaying or auditing
	 * a wave needs to know which objects changed and whether the whole thing landed
	 * as one undo entry -- neither is inferable from a list of function names.
	 */
	/**
	 * Whether the transaction system can actually restore this object.
	 *
	 * Transient objects are outside undo entirely: Modify() records nothing and
	 * Cancel() has nothing to put back. A dynamic mesh session object is exactly
	 * that, so a receipt built without this check reported rolled_back:true on a
	 * mesh that had in fact been left transformed -- the caller would trust the
	 * rollback and act on a state that never reverted.
	 */
	inline bool IsUndoable(const UObject* Object)
	{
		return Object != nullptr
			&& Object->HasAnyFlags(RF_Transactional)
			&& !Object->HasAnyFlags(RF_Transient)
			&& Object->GetOutermost() != GetTransientPackage();
	}

	inline TSharedRef<FJsonObject> MakeBatchReceipt(
		const TArray<UObject*>& Touched,
		const int32 Mutating,
		const bool bTransactional,
		const bool bCancelled)
	{
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("schema"), TEXT("somol.reflection_batch_receipt:v3"));
		Receipt->SetNumberField(TEXT("mutating_calls"), Mutating);
		Receipt->SetBoolField(TEXT("transactional"), bTransactional);

		TArray<TSharedPtr<FJsonValue>> Objects;
		int32 Undoable = 0;
		int32 NotUndoable = 0;
		for (const UObject* Object : Touched)
		{
			if (Object == nullptr)
			{
				continue;
			}
			const bool bUndoable = IsUndoable(Object);
			Undoable += bUndoable ? 1 : 0;
			NotUndoable += bUndoable ? 0 : 1;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), Object->GetPathName());
			Row->SetBoolField(TEXT("undoable"), bUndoable);
			Objects.Add(MakeShared<FJsonValueObject>(Row));
		}
		// "recorded" rather than "touched": these are the objects Modify() was called
		// on, which is what the transaction covers -- not necessarily everything the
		// call changed.
		Receipt->SetArrayField(TEXT("recorded_objects"), Objects);
		Receipt->SetNumberField(TEXT("touched_count"), Objects.Num());
		Receipt->SetNumberField(TEXT("undoable_objects"), Undoable);
		Receipt->SetNumberField(TEXT("non_undoable_objects"), NotUndoable);

		// Report the action taken, never a rollback guarantee.
		//
		// A generic dispatcher cannot know which objects a UFUNCTION will modify. It
		// can only call Modify() on the objects it passed in, and that is frequently
		// the wrong set: SetDisplayRate(Sequence) mutates Sequence->GetMovieScene(),
		// an object that never appears in the signature. Cancelling the transaction
		// then restores the recorded object, which did not change, while the one that
		// did keeps its new value.
		//
		// Measured on 5.8: a cancelled batch left a sequence at 96fps after reporting
		// a successful rollback. Claiming rolled_back here would be a guarantee this
		// mechanism cannot make, and a caller acting on it would be working from state
		// that never reverted. So the receipt states what was done and what was
		// covered, and tells the caller to verify by reading the value back.
		Receipt->SetBoolField(TEXT("transaction_cancelled"), bCancelled);
		Receipt->SetBoolField(TEXT("rollback_guaranteed"), false);
		if (bCancelled)
		{
			Receipt->SetStringField(TEXT("rollback_note"),
				TEXT("The transaction was cancelled, but rollback is not guaranteed: a generic call "
					 "can modify objects that were never parameters, and only the objects listed "
					 "here were recorded. Verify by reading the value back; for a dynamic mesh "
					 "session, reopen it from its asset."));
		}
		if (NotUndoable > 0)
		{
			Receipt->SetStringField(TEXT("transient_note"),
				TEXT("Some touched objects are transient and outside the transaction system "
					 "entirely, so nothing about them undoes."));
		}
		if (bTransactional && Mutating > 0 && !bCancelled)
		{
			Receipt->SetStringField(TEXT("undo"), NotUndoable == 0
				? TEXT("The batch ran inside one transaction, so what the editor recorded undoes as "
					   "one entry. Coverage is best-effort: objects the call touched indirectly may "
					   "not be included.")
				: TEXT("Undo covers the persistent objects only; transient session objects are "
					   "outside the transaction system."));
		}
		else if (!bTransactional && Mutating > 0)
		{
			Receipt->SetStringField(TEXT("undo"),
				TEXT("Not transactional: calls undo individually, if at all."));
		}
		return Receipt;
	}

	/** Run one call within a domain. */
	inline bool RunCall(
		const FDomain& Domain,
		const FString& FunctionName,
		const FString& TargetRef,
		const TSharedPtr<FJsonObject>& Params,
		const TSharedRef<FJsonObject>& OutResult,
		FString& OutError,
		TArray<UObject*>* OutTouched = nullptr)
	{
		UClass* OwnerClass = nullptr;
		bool bAmbiguous = false;
		UFunction* Function = FindDomainFunction(Domain, FunctionName, OwnerClass, bAmbiguous);
		if (Function == nullptr)
		{
			OutResult->SetBoolField(TEXT("ok"), false);
			OutResult->SetStringField(TEXT("error_code"), bAmbiguous ? TEXT("AMBIGUOUS_NAME") : TEXT("NOT_FOUND"));
			OutResult->SetStringField(TEXT("error"), bAmbiguous
				? FString::Printf(TEXT("'%s' is defined by more than one class in domain '%s'. "
									   "Qualify it as Class.Function."), *FunctionName, *DomainLabel(Domain))
				: FString::Printf(TEXT("Domain '%s' has no callable function '%s'. Run "
									   "editor_api_catalog to list what this editor actually has."),
					*DomainLabel(Domain), *FunctionName));
			OutError = FString::Printf(TEXT("Unresolved function '%s'."), *FunctionName);
			return false;
		}

		const bool bStatic = Function->HasAnyFunctionFlags(FUNC_Static);
		UObject* Target = nullptr;
		if (!TargetRef.IsEmpty())
		{
			FString ResolveError;
			Target = ResolveRef(TargetRef, OwnerClass, ResolveError);
			if (Target == nullptr)
			{
				OutResult->SetBoolField(TEXT("ok"), false);
				OutResult->SetStringField(TEXT("error_code"), TEXT("NOT_FOUND"));
				OutResult->SetStringField(TEXT("error"), ResolveError);
				OutError = ResolveError;
				return false;
			}
		}
		else if (bStatic)
		{
			Target = OwnerClass->GetDefaultObject();
		}
		else
		{
			// Calling an instance method on the CDO would appear to work and quietly
			// mutate shared default state, so it is refused rather than defaulted.
			OutResult->SetBoolField(TEXT("ok"), false);
			OutResult->SetStringField(TEXT("error_code"), TEXT("MISSING_PARAM"));
			OutResult->SetStringField(TEXT("error"),
				FString::Printf(TEXT("'%s' is an instance method on %s and needs a target: pass an "
									 "asset path, or a handle returned by an earlier call."),
					*Function->GetName(), *OwnerClass->GetName()));
			OutError = FString::Printf(TEXT("Function '%s' needs a target."), *Function->GetName());
			return false;
		}

		const FString Origin = FString::Printf(TEXT("%s.%s"), Domain.Name, *Function->GetName());
		const bool bOk = Reflection::InvokeFunction(
			Target, Function, Params, MakePolicy(Domain, Origin), OutResult, OutError,
			Reflection::FPostInvoke(), OutTouched);

		OutResult->SetStringField(TEXT("function"),
			FString::Printf(TEXT("%s.%s"), *OwnerClass->GetName(), *Function->GetName()));
		OutResult->SetStringField(TEXT("domain"), DomainLabel(Domain));
		if (Target != nullptr && !bStatic)
		{
			OutResult->SetStringField(TEXT("target"), TargetRef);
		}
		return bOk;
	}
} // namespace EditorApiToolsPrivate

void RegisterEditorApiTools(FSololmcpToolRegistry& Registry)
{
	using namespace EditorApiToolsPrivate;

	// ── editor_api_domains ─────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_api_domains"),
		TEXT("List the plugin API domains this editor exposes — DynamicMaterial, ClonerEffector, USD "
			 "and Datasmith — with how many classes and callable functions each actually has here. A "
			 "domain whose plugin is disabled reports zero rather than failing, so this is the way to "
			 "check availability before planning against it."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("include_modules"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Also list every module with a callable count. Each is addressable as "
							 "domain=\"module:<ModuleName>\", which is how the surface outside the "
							 "curated domains is reached.")), false)}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 AvailableCount = 0;
			int32 TotalFunctions = 0;

			for (const FDomain& Domain : GetDomains())
			{
				const TArray<UClass*> Classes = GetDomainClasses(Domain);
				int32 Functions = 0;
				int32 StaticFunctions = 0;
				for (UClass* Class : Classes)
				{
					for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
					{
						if (!IsExposedFunction(*It))
						{
							continue;
						}
						++Functions;
						StaticFunctions += It->HasAnyFunctionFlags(FUNC_Static) ? 1 : 0;
					}
				}

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("domain"), Domain.Name);
				Row->SetStringField(TEXT("plugin"), Domain.Plugin);
				Row->SetBoolField(TEXT("available"), Classes.Num() > 0);
				Row->SetNumberField(TEXT("classes"), Classes.Num());
				Row->SetNumberField(TEXT("functions"), Functions);
				Row->SetNumberField(TEXT("static_functions"), StaticFunctions);
				Row->SetNumberField(TEXT("instance_functions"), Functions - StaticFunctions);
				Row->SetStringField(TEXT("handle_kind"), HandleKindFor(Domain));
				Row->SetStringField(TEXT("note"), Domain.Note);
				if (Classes.Num() == 0)
				{
					Row->SetStringField(TEXT("reason"),
						TEXT("No classes registered — the plugin is not enabled in this project."));
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));

				AvailableCount += Classes.Num() > 0 ? 1 : 0;
				TotalFunctions += Functions;
			}

			// The curated domains are ~12 of ~380 modules. Listing every module with a
			// callable count is what makes module: addressing usable rather than a
			// thing you have to already know about, and it is the only way a caller
			// can see the whole surface.
			bool bIncludeModules = false;
			Args->TryGetBoolField(TEXT("include_modules"), bIncludeModules);
			if (bIncludeModules)
			{
				TMap<FString, int32> ByModule;
				for (TObjectIterator<UClass> It; It; ++It)
				{
					UClass* Class = *It;
					if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
					{
						continue;
					}
					const FString Module = GetOwningModule(Class);
					if (Module.IsEmpty())
					{
						continue;
					}
					int32 Functions = 0;
					for (TFieldIterator<UFunction> Fn(Class, EFieldIteratorFlags::ExcludeSuper); Fn; ++Fn)
					{
						Functions += IsExposedFunction(*Fn) ? 1 : 0;
					}
					if (Functions > 0)
					{
						ByModule.FindOrAdd(Module) += Functions;
					}
				}
				TArray<FString> ModuleNames;
				ByModule.GetKeys(ModuleNames);
				ModuleNames.Sort();
				TArray<TSharedPtr<FJsonValue>> ModuleRows;
				int32 ModuleFunctions = 0;
				for (const FString& Module : ModuleNames)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("module"), Module);
					Row->SetStringField(TEXT("domain"), FString::Printf(TEXT("module:%s"), *Module));
					Row->SetNumberField(TEXT("functions"), ByModule[Module]);
					ModuleRows.Add(MakeShared<FJsonValueObject>(Row));
					ModuleFunctions += ByModule[Module];
				}
				OutStructured->SetArrayField(TEXT("modules"), ModuleRows);
				OutStructured->SetNumberField(TEXT("module_count"), ModuleRows.Num());
				OutStructured->SetNumberField(TEXT("module_functions_total"), ModuleFunctions);
			}
			else
			{
				OutStructured->SetStringField(TEXT("modules_hint"),
					TEXT("Pass include_modules=true for every module and its callable count. Any "
						 "module is addressable as domain=\"module:<ModuleName>\"."));
			}

			OutStructured->SetArrayField(TEXT("domains"), Rows);
			OutStructured->SetNumberField(TEXT("available"), AvailableCount);
			OutStructured->SetNumberField(TEXT("total_functions"), TotalFunctions);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d of %d domain(s) available; %d callable function(s)."),
				AvailableCount, GetDomains().Num(), TotalFunctions);
			return true;
		},
		nullptr,
		0
	});

	// ── editor_api_catalog ─────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_api_catalog"),
		TEXT("List callable functions in a plugin API domain, with their parameters and whether each "
			 "is reachable through editor_api_call. Functions taking a live scene object that has no "
			 "handle are reported callable=false, so an unreachable call is caught while planning "
			 "instead of mid-wave."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("domain"), FSololmcpSchemaBuilder::WithEnum(
					FSololmcpSchemaBuilder::String(TEXT("Which domain to list.")),
					{TEXT("dynamic_material"), TEXT("cloner_effector"), TEXT("usd"), TEXT("datasmith"),
					 TEXT("sequencer"), TEXT("movie_render"), TEXT("take_recorder"),
					 TEXT("umg"), TEXT("anim_rig"), TEXT("motion_design"), TEXT("audio"), TEXT("pcg")})},
				{TEXT("class"), FSololmcpSchemaBuilder::String(
					TEXT("Restrict to one class, e.g. DMMaterialSlot. Omit for the whole domain."))},
				{TEXT("search"), FSololmcpSchemaBuilder::String(
					TEXT("Substring match on the function name."))},
				{TEXT("static_only"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Only functions callable without a target object.")), false)},
				{TEXT("include_params"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Include each function's parameter list.")), false)},
				{TEXT("limit"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(TEXT("Maximum functions to return.")), 200)}
			},
			{TEXT("domain")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString DomainName;
			if (!Args->TryGetStringField(TEXT("domain"), DomainName) || DomainName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("domain"));
				OutError = TEXT("Missing domain.");
				return false;
			}
			FDomain Resolved;
			FString ResolveError;
			if (!ResolveDomain(DomainName, Resolved, ResolveError))
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_PARAM"), TEXT("domain"), ResolveError);
				OutError = ResolveError;
				return false;
			}
			const FDomain* Domain = &Resolved;

			FString ClassFilter;
			Args->TryGetStringField(TEXT("class"), ClassFilter);
			FString Search;
			Args->TryGetStringField(TEXT("search"), Search);
			bool bStaticOnly = false;
			Args->TryGetBoolField(TEXT("static_only"), bStaticOnly);
			bool bIncludeParams = false;
			Args->TryGetBoolField(TEXT("include_params"), bIncludeParams);
			int32 Limit = 200;
			Args->TryGetNumberField(TEXT("limit"), Limit);
			Limit = FMath::Clamp(Limit, 1, 2000);

			const Reflection::FInvokePolicy DescribePolicy = MakePolicy(*Domain, FString());
			const TArray<UClass*> Classes = GetDomainClasses(*Domain);

			TArray<TSharedPtr<FJsonValue>> Rows;
			TArray<TSharedPtr<FJsonValue>> ClassNames;
			int32 Matched = 0;
			int32 NotCallable = 0;

			for (UClass* Class : Classes)
			{
				const FString ClassName = Class->GetName();
				ClassNames.Add(MakeShared<FJsonValueString>(ClassName));
				if (!ClassFilter.IsEmpty() && !ClassName.Equals(ClassFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
				{
					UFunction* Function = *It;
					if (!IsExposedFunction(Function))
					{
						continue;
					}
					if (bStaticOnly && !Function->HasAnyFunctionFlags(FUNC_Static))
					{
						continue;
					}
					if (!Search.IsEmpty() && !Function->GetName().Contains(Search))
					{
						continue;
					}
					++Matched;
					TSharedRef<FJsonObject> Described = Reflection::DescribeFunction(
						Class, Function, DescribePolicy, bIncludeParams, ClassName);
					Described->SetStringField(TEXT("module"), GetOwningModule(Class));
					bool bCallable = true;
					Described->TryGetBoolField(TEXT("callable"), bCallable);
					NotCallable += bCallable ? 0 : 1;
					if (Rows.Num() < Limit)
					{
						Rows.Add(MakeShared<FJsonValueObject>(Described));
					}
				}
			}

			OutStructured->SetStringField(TEXT("domain"), DomainLabel(*Domain));
			OutStructured->SetBoolField(TEXT("available"), Classes.Num() > 0);
			OutStructured->SetArrayField(TEXT("functions"), Rows);
			OutStructured->SetArrayField(TEXT("classes"), ClassNames);
			OutStructured->SetNumberField(TEXT("returned"), Rows.Num());
			OutStructured->SetNumberField(TEXT("matched"), Matched);
			OutStructured->SetNumberField(TEXT("not_callable"), NotCallable);
			OutStructured->SetBoolField(TEXT("truncated"), Matched > Rows.Num());
			if (Classes.Num() == 0)
			{
				OutStructured->SetStringField(TEXT("reason"),
					TEXT("No classes registered — the plugin is not enabled in this project."));
			}
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = Classes.Num() == 0
				? FString::Printf(TEXT("Domain '%s' is not available in this editor."), *DomainLabel(*Domain))
				: FString::Printf(TEXT("%d function(s) matched in '%s'; returned %d, %d not callable."),
					Matched, *DomainLabel(*Domain), Rows.Num(), NotCallable);
			return true;
		},
		nullptr,
		0
	});

	// ── editor_api_call ────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_api_call"),
		TEXT("Call one function in a plugin API domain. Static functions need no target; instance "
			 "functions need one, given as an asset path or as a handle returned by an earlier call. "
			 "Objects returned by a call are registered as handles, which is how you walk from a "
			 "material model to its slots, layers and stages."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("domain"), FSololmcpSchemaBuilder::WithEnum(
					FSololmcpSchemaBuilder::String(TEXT("Which domain the function belongs to.")),
					{TEXT("dynamic_material"), TEXT("cloner_effector"), TEXT("usd"), TEXT("datasmith"),
					 TEXT("sequencer"), TEXT("movie_render"), TEXT("take_recorder"),
					 TEXT("umg"), TEXT("anim_rig"), TEXT("motion_design"), TEXT("audio"), TEXT("pcg")})},
				{TEXT("function"), FSololmcpSchemaBuilder::String(
					TEXT("Function name, qualified as Class.Function when the bare name is ambiguous."))},
				{TEXT("target"), FSololmcpSchemaBuilder::String(
					TEXT("Asset path or session handle to call on. Omit for static functions."))},
				{TEXT("params"), FSololmcpSchemaBuilder::Object(
					{}, {}, TEXT("Parameter values keyed by engine parameter name."))}
			},
			{TEXT("domain"), TEXT("function")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString DomainName;
			Args->TryGetStringField(TEXT("domain"), DomainName);
			FDomain Resolved;
			FString ResolveError;
			if (!ResolveDomain(DomainName, Resolved, ResolveError))
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_PARAM"), TEXT("domain"), ResolveError);
				OutError = ResolveError;
				return false;
			}
			const FDomain* Domain = &Resolved;
			FString FunctionName;
			if (!Args->TryGetStringField(TEXT("function"), FunctionName) || FunctionName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("function"));
				OutError = TEXT("Missing function.");
				return false;
			}

			FString TargetRef;
			Args->TryGetStringField(TEXT("target"), TargetRef);
			const TSharedPtr<FJsonObject>* Params = nullptr;
			Args->TryGetObjectField(TEXT("params"), Params);

			const bool bOk = RunCall(*Domain, FunctionName, TargetRef,
				Params != nullptr ? *Params : nullptr, OutStructured, OutError);
			OutSummary = bOk
				? FString::Printf(TEXT("Called %s in '%s'."), *FunctionName, *DomainLabel(*Domain))
				: FString::Printf(TEXT("%s failed in '%s'."), *FunctionName, *DomainLabel(*Domain));
			return bOk;
		},
		nullptr,
		0
	});

	// ── editor_api_call_batch ──────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_api_call_batch"),
		TEXT("Run a sequence of plugin API calls in one queue entry. Each entry may reference a "
			 "handle produced by an earlier entry through $prev, which is what lets a whole object "
			 "graph walk — model to slot to layer — happen without a round trip per step."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("domain"), FSololmcpSchemaBuilder::WithEnum(
					FSololmcpSchemaBuilder::String(TEXT("Default domain for entries.")),
					{TEXT("dynamic_material"), TEXT("cloner_effector"), TEXT("usd"), TEXT("datasmith"),
					 TEXT("sequencer"), TEXT("movie_render"), TEXT("take_recorder"),
					 TEXT("umg"), TEXT("anim_rig"), TEXT("motion_design"), TEXT("audio"), TEXT("pcg")})},
				{TEXT("calls"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object(
						{
							{TEXT("function"), FSololmcpSchemaBuilder::String(TEXT("Function name."))},
							{TEXT("domain"), FSololmcpSchemaBuilder::String(
								TEXT("Override the default domain for this entry."))},
							{TEXT("target"), FSololmcpSchemaBuilder::String(
								TEXT("Asset path or handle. Use $prev to reuse the handle the previous "
									 "entry returned, or $prev.FieldName for a specific output."))},
							{TEXT("params"), FSololmcpSchemaBuilder::Object(
								{}, {}, TEXT("Parameter values."))}
						},
						{TEXT("function")}),
					TEXT("Calls to run in order."))},
				{TEXT("stop_on_error"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Stop at the first failure. On by default, because a later entry usually "
							 "depends on what an earlier one returned.")), true)},
				{TEXT("transaction"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Run the batch inside one transaction, so it is a single undo entry and "
							 "a batch stopped by an error is rolled back rather than half-applied. "
							 "Turn off only when the calls are read-only.")), true)}
			},
			{TEXT("calls")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const TArray<TSharedPtr<FJsonValue>>* Calls = nullptr;
			if (!Args->TryGetArrayField(TEXT("calls"), Calls) || Calls == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("calls"));
				OutError = TEXT("Missing calls array.");
				return false;
			}

			FString DefaultDomain;
			Args->TryGetStringField(TEXT("domain"), DefaultDomain);
			bool bStopOnError = true;
			Args->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

			bool bTransactional = true;
			Args->TryGetBoolField(TEXT("transaction"), bTransactional);

			// One transaction for the whole batch, so a wave of calls is a single undo
			// entry rather than N of them -- or none, which is what happens when a
			// generic dispatcher invokes a mutating UFUNCTION outside any transaction.
			TUniquePtr<FScopedTransaction> Transaction;
			if (bTransactional)
			{
				Transaction = MakeUnique<FScopedTransaction>(
					NSLOCTEXT("SOMOLMCP", "EditorApiCallBatch", "SOMOLMCP Editor API Batch"));
			}
			TArray<UObject*> Touched;
			int32 MutatingCalls = 0;

			TArray<TSharedPtr<FJsonValue>> Results;
			TSharedPtr<FJsonObject> Previous;
			int32 Succeeded = 0;
			int32 Failed = 0;
			bool bStopped = false;

			for (int32 Index = 0; Index < Calls->Num(); ++Index)
			{
				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetNumberField(TEXT("index"), Index);

				const TSharedPtr<FJsonObject>* Entry = nullptr;
				FString FunctionName;
				if ((*Calls)[Index]->TryGetObject(Entry) && Entry != nullptr)
				{
					(*Entry)->TryGetStringField(TEXT("function"), FunctionName);
				}
				if (Entry == nullptr || FunctionName.IsEmpty())
				{
					Result->SetBoolField(TEXT("ok"), false);
					Result->SetStringField(TEXT("error_code"), TEXT("INVALID_PARAM"));
					Result->SetStringField(TEXT("error"), TEXT("Entry needs an object with a function."));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					++Failed;
					if (bStopOnError) { bStopped = true; break; }
					continue;
				}

				FString EntryDomain = DefaultDomain;
				(*Entry)->TryGetStringField(TEXT("domain"), EntryDomain);
				FDomain ResolvedEntry;
				FString EntryResolveError;
				const FDomain* Domain = ResolveDomain(EntryDomain, ResolvedEntry, EntryResolveError)
					? &ResolvedEntry : nullptr;
				if (Domain == nullptr)
				{
					Result->SetBoolField(TEXT("ok"), false);
					Result->SetStringField(TEXT("error_code"), TEXT("INVALID_PARAM"));
					Result->SetStringField(TEXT("error"), EntryResolveError);
					Results.Add(MakeShared<FJsonValueObject>(Result));
					++Failed;
					if (bStopOnError) { bStopped = true; break; }
					continue;
				}

				FString TargetRef;
				(*Entry)->TryGetStringField(TEXT("target"), TargetRef);

				// $prev threading. Without it every step of an object-graph walk would
				// have to come back to the client for the handle it just produced,
				// which is exactly the round trip batching exists to remove.
				if (TargetRef.StartsWith(TEXT("$prev")))
				{
					FString Resolved;
					if (Previous.IsValid())
					{
						FString Field;
						if (TargetRef.Split(TEXT("."), nullptr, &Field) && !Field.IsEmpty())
						{
							Previous->TryGetStringField(Field, Resolved);
						}
						else
						{
							// No field named: use the sole object the previous call
							// returned. Guessing among several would be worse than
							// making the caller name one.
							FString OnlyHandle;
							int32 HandleCount = 0;
							for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Previous->Values)
							{
								FString AsString;
								if (Pair.Value.IsValid() && Pair.Value->TryGetString(AsString)
									&& FSololmcpObjectHandles::Get().Find(AsString) != nullptr)
								{
									OnlyHandle = AsString;
									++HandleCount;
								}
							}
							if (HandleCount == 1)
							{
								Resolved = OnlyHandle;
							}
						}
					}
					if (Resolved.IsEmpty())
					{
						Result->SetBoolField(TEXT("ok"), false);
						Result->SetStringField(TEXT("error_code"), TEXT("NOT_FOUND"));
						Result->SetStringField(TEXT("error"),
							FString::Printf(TEXT("'%s' could not be resolved from the previous result. "
												 "Name the output field explicitly, as $prev.FieldName."),
								*TargetRef));
						Results.Add(MakeShared<FJsonValueObject>(Result));
						++Failed;
						if (bStopOnError) { bStopped = true; break; }
						continue;
					}
					TargetRef = Resolved;
				}

				const TSharedPtr<FJsonObject>* Params = nullptr;
				(*Entry)->TryGetObjectField(TEXT("params"), Params);

				FString StepError;
				const bool bOk = RunCall(*Domain, FunctionName, TargetRef,
					Params != nullptr ? *Params : nullptr, Result, StepError, &Touched);
				bool bMutated = false;
				Result->TryGetBoolField(TEXT("mutating"), bMutated);
				MutatingCalls += bMutated ? 1 : 0;
				Results.Add(MakeShared<FJsonValueObject>(Result));
				Previous = Result;

				if (bOk)
				{
					++Succeeded;
				}
				else
				{
					++Failed;
					if (bStopOnError) { bStopped = true; break; }
				}
			}

			// Roll back a partial batch. Leaving half a wave applied is worse than
			// applying none of it: the caller cannot tell from the result which half
			// landed, and a retry would double-apply the successful steps.
			const bool bRollback = bTransactional && bStopped && MutatingCalls > 0;
			if (bRollback && Transaction.IsValid())
			{
				Transaction->Cancel();
			}
			Transaction.Reset();

			OutStructured->SetObjectField(TEXT("receipt"),
				MakeBatchReceipt(Touched, MutatingCalls, bTransactional, bRollback));
			OutStructured->SetArrayField(TEXT("results"), Results);
			OutStructured->SetNumberField(TEXT("requested"), Calls->Num());
			OutStructured->SetNumberField(TEXT("succeeded"), Succeeded);
			OutStructured->SetNumberField(TEXT("failed"), Failed);
			OutStructured->SetBoolField(TEXT("stopped_early"), bStopped);
			if (bStopped)
			{
				OutStructured->SetNumberField(TEXT("not_attempted"), Calls->Num() - Results.Num());
			}
			OutStructured->SetBoolField(TEXT("ok"), Failed == 0);
			OutSummary = FString::Printf(TEXT("%d of %d call(s) succeeded%s."),
				Succeeded, Calls->Num(), bStopped ? TEXT("; stopped at the first failure") : TEXT(""));
			if (Failed > 0)
			{
				OutError = FString::Printf(TEXT("%d call(s) failed."), Failed);
			}
			return Failed == 0;
		},
		nullptr,
		0
	});
}

} // namespace UE::SOMOLMCP
