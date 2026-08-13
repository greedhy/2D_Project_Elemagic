// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

// Shared machinery for invoking UFUNCTIONs from JSON.
//
// Several large editor surfaces are shaped the same way: hundreds of
// BlueprintCallable functions whose only real variation is which class they hang
// off and what their parameters are. Writing a tool per function does not scale --
// GeometryScripting alone is 509 of them -- and writing a dispatcher per plugin
// duplicates the same ~200 lines of parameter marshalling each time.
//
// This header is that marshalling, once. A domain supplies three policies:
//
//   ResolveObject  how a JSON string names an object in this domain (an asset
//                  path, a session handle, or both). Object parameters are the
//                  one thing reflection cannot do generically, because "which
//                  object" is domain knowledge.
//   AutoSupply     parameter classes the domain fills in itself rather than
//                  asking the caller for -- GeometryScripting's debug object is
//                  the motivating case.
//   MintHandle     what to do with a returned live object. Domains that hand back
//                  transient objects register them; domains that do not return
//                  an empty string and the value is reported as a plain object name.
//
// Everything else -- frame lifetime, struct and array marshalling, out-parameter
// extraction, catalog description -- is identical across domains and lives here.
//
// Scope note: this is deliberately *not* a general "call any UFUNCTION" facility.
// Each caller passes an explicit class filter, so a domain tool can only reach the
// classes it declares. The unrestricted generic invoke path already exists
// elsewhere and is gated read-only with its own allowlist; nothing here widens it.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "Components/ActorComponent.h"

namespace UE::SOMOLMCP::Reflection
{
	/** Outcome detail a domain can attach after a call (log messages, receipts). */
	using FPostInvoke = TFunction<int32(const TSharedRef<FJsonObject>& OutResult)>;

	/** How one domain resolves object-typed parameters and returns. */
	struct FInvokePolicy
	{
		/**
		 * Turn a JSON string into an object of (at most) ExpectedClass.
		 * Return null and set OutError to reject. Required.
		 */
		TFunction<UObject*(const FString& Ref, UClass* ExpectedClass, FString& OutError)> ResolveObject;

		/**
		 * Fill a parameter the caller did not name, by class. Return null to leave it
		 * alone. Optional.
		 */
		TFunction<UObject*(UClass* ParamClass)> AutoSupply;

		/**
		 * Register a returned live object and return its handle, or empty to just
		 * report the object's name. Optional.
		 */
		TFunction<FString(UObject* Returned)> MintHandle;

		/**
		 * A subject filled into any object parameter the caller did not name and
		 * AutoSupply did not claim -- the "apply this to that" default. Optional.
		 */
		UObject* DefaultSubject = nullptr;
		UClass* DefaultSubjectClass = nullptr;

		/**
		 * Whether ResolveObject can name live scene objects — that is, whether this
		 * domain registers handles for objects returned by earlier calls.
		 *
		 * This only affects what the catalog advertises, but getting it wrong is not
		 * harmless in either direction: false when handles do work reports callable
		 * functions as unreachable and callers skip them, while true when they do not
		 * promises a call that will fail at execution time. Domains whose handles only
		 * ever hold one specific type should leave this false.
		 */
		bool bHandlesLiveObjects = false;
	};

	/**
	 * Whether calling this function can change state.
	 *
	 * Pure and const functions are declared not to mutate; everything else is
	 * assumed to. The asymmetry is deliberate: treating a mutating function as
	 * read-only would leave it outside the transaction and silently unundoable,
	 * whereas treating a read-only one as mutating costs only an empty Modify().
	 */
	inline bool IsMutatingFunction(const UFunction* Function)
	{
		return Function != nullptr
			&& !Function->HasAnyFunctionFlags(FUNC_BlueprintPure | FUNC_Const);
	}

	inline bool IsOutputParam(const FProperty* Property)
	{
		if (!Property->HasAnyPropertyFlags(CPF_Parm))
		{
			return false;
		}
		if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			return true;
		}
		return Property->HasAnyPropertyFlags(CPF_OutParm)
			&& !Property->HasAnyPropertyFlags(CPF_ConstParm);
	}

	/** True for object classes that have an asset path, as opposed to live scene objects. */
	inline bool IsAssetLikeClass(const UClass* Class)
	{
		return Class != nullptr && !Class->IsChildOf(UActorComponent::StaticClass());
	}

	/** Describe one parameter for a catalog listing. */
	inline TSharedRef<FJsonObject> DescribeParam(const FProperty* Property, const FInvokePolicy& Policy)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Property->GetName());
		Obj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		Obj->SetBoolField(TEXT("is_output"), IsOutputParam(Property));
		Obj->SetBoolField(TEXT("is_return"), Property->HasAnyPropertyFlags(CPF_ReturnParm));

		if (const FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(Property))
		{
			UClass* ParamClass = AsObject->PropertyClass;
			Obj->SetStringField(TEXT("class"), ParamClass != nullptr ? ParamClass->GetName() : TEXT("Object"));

			if (Policy.AutoSupply && ParamClass != nullptr && Policy.AutoSupply(ParamClass) != nullptr)
			{
				Obj->SetStringField(TEXT("role"), TEXT("auto"));
				Obj->SetStringField(TEXT("note"), TEXT("Supplied automatically; do not pass it."));
				Obj->SetBoolField(TEXT("supported"), true);
			}
			else if (Policy.DefaultSubjectClass != nullptr && ParamClass != nullptr
				&& ParamClass->IsChildOf(Policy.DefaultSubjectClass))
			{
				Obj->SetStringField(TEXT("role"), TEXT("subject"));
				Obj->SetStringField(TEXT("note"),
					TEXT("Defaults to the subject named on the tool; pass a reference to override."));
				Obj->SetBoolField(TEXT("supported"), true);
			}
			else
			{
				const bool bAssetLike = IsAssetLikeClass(ParamClass);
				const bool bSupported = bAssetLike || Policy.bHandlesLiveObjects;
				Obj->SetStringField(TEXT("role"), bAssetLike ? TEXT("object") : TEXT("scene_object"));
				Obj->SetStringField(TEXT("note"), bAssetLike
					? TEXT("Pass an asset path or a session handle as a string.")
					: bSupported
						? TEXT("A live scene object: pass a handle returned by an earlier call. It "
							   "has no asset path, so a path will not resolve it.")
						: TEXT("This is a live scene object with no asset path, so this function is "
							   "not reachable through the generic dispatcher."));
				Obj->SetBoolField(TEXT("supported"), bSupported);
			}
		}
		else if (const FStructProperty* AsStruct = CastField<FStructProperty>(Property))
		{
			Obj->SetStringField(TEXT("role"), TEXT("struct"));
			Obj->SetStringField(TEXT("struct"), AsStruct->Struct->GetName());
			Obj->SetStringField(TEXT("note"),
				TEXT("Pass a JSON object with the fields you want to override; omitted fields keep "
					 "their engine defaults."));
			Obj->SetBoolField(TEXT("supported"), true);

			// Name the fields.
			//
			// Reporting only the struct's type name forces the caller to already know
			// its layout -- in practice, to read the engine header. A caller who
			// guesses wrong does not get an error: JsonValueToUProperty leaves the
			// unmatched field at its default and the call returns success, so a wrong
			// guess looks exactly like a correct one. Listing the fields is what makes
			// this catalog self-sufficient rather than a pointer to the headers.
			TArray<TSharedPtr<FJsonValue>> Fields;
			for (TFieldIterator<FProperty> It(AsStruct->Struct); It; ++It)
			{
				TSharedRef<FJsonObject> Field = MakeShared<FJsonObject>();
				Field->SetStringField(TEXT("name"), It->GetName());
				Field->SetStringField(TEXT("type"), It->GetCPPType());
				// One level deep only: nested structs would make a catalog page
				// unbounded, and the common case is flat option structs.
				if (const FStructProperty* Nested = CastField<FStructProperty>(*It))
				{
					Field->SetStringField(TEXT("struct"), Nested->Struct->GetName());
					Field->SetBoolField(TEXT("nested"), true);
				}
				Fields.Add(MakeShared<FJsonValueObject>(Field));
			}
			Obj->SetArrayField(TEXT("fields"), Fields);
		}
		else
		{
			Obj->SetStringField(TEXT("role"), TEXT("value"));
			Obj->SetBoolField(TEXT("supported"), true);
		}
		return Obj;
	}

	/** Describe a function for a catalog listing. */
	inline TSharedRef<FJsonObject> DescribeFunction(
		const UClass* OwnerClass,
		UFunction* Function,
		const FInvokePolicy& Policy,
		const bool bIncludeParams,
		const FString& OwnerLabel)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), FString::Printf(TEXT("%s.%s"), *OwnerLabel, *Function->GetName()));
		Obj->SetStringField(TEXT("owner"), OwnerLabel);
		Obj->SetStringField(TEXT("class"), OwnerClass->GetName());
		Obj->SetBoolField(TEXT("is_static"), Function->HasAnyFunctionFlags(FUNC_Static));

#if WITH_EDITOR
		const FString Tooltip = Function->GetToolTipText().ToString();
		if (!Tooltip.IsEmpty())
		{
			Obj->SetStringField(TEXT("summary"), Tooltip.Left(400));
		}
#endif

		bool bAllSupported = true;
		if (bIncludeParams)
		{
			TArray<TSharedPtr<FJsonValue>> Inputs;
			TArray<TSharedPtr<FJsonValue>> Outputs;
			for (TFieldIterator<FProperty> It(Function); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property->HasAnyPropertyFlags(CPF_Parm))
				{
					continue;
				}
				TSharedRef<FJsonObject> Described = DescribeParam(Property, Policy);
				bool bSupported = true;
				Described->TryGetBoolField(TEXT("supported"), bSupported);
				bAllSupported &= bSupported;
				(IsOutputParam(Property) ? Outputs : Inputs).Add(MakeShared<FJsonValueObject>(Described));
			}
			Obj->SetArrayField(TEXT("inputs"), Inputs);
			Obj->SetArrayField(TEXT("outputs"), Outputs);
		}
		else
		{
			for (TFieldIterator<FProperty> It(Function); It && bAllSupported; ++It)
			{
				const FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(*It);
				if (AsObject == nullptr || !It->HasAnyPropertyFlags(CPF_Parm))
				{
					continue;
				}
				UClass* ParamClass = AsObject->PropertyClass;
				const bool bAuto = Policy.AutoSupply && ParamClass && Policy.AutoSupply(ParamClass) != nullptr;
				const bool bSubject = Policy.DefaultSubjectClass && ParamClass
					&& ParamClass->IsChildOf(Policy.DefaultSubjectClass);
				bAllSupported &= bAuto || bSubject || Policy.bHandlesLiveObjects || IsAssetLikeClass(ParamClass);
			}
		}
		// Callers plan against this: it is the difference between "this function
		// exists" and "you can actually call it from here".
		Obj->SetBoolField(TEXT("callable"), bAllSupported);
		return Obj;
	}

	/**
	 * Invoke Function on Target, filling parameters from Params.
	 *
	 * Writes results into OutResult and returns false with OutError set on any
	 * failure. PostInvoke, when supplied, runs after the call and returns a count of
	 * domain-detected errors; a non-zero count turns a call that "succeeded" into a
	 * reported failure. That matters because several of these APIs report problems
	 * through a side channel and return nothing, so without it a queued wave would
	 * read a no-op as success.
	 */
	inline bool InvokeFunction(
		UObject* Target,
		UFunction* Function,
		const TSharedPtr<FJsonObject>& Params,
		const FInvokePolicy& Policy,
		const TSharedRef<FJsonObject>& OutResult,
		FString& OutError,
		const FPostInvoke& PostInvoke = FPostInvoke(),
		TArray<UObject*>* OutTouchedObjects = nullptr)
	{
		TArray<UObject*> LocalTouched;
		TArray<UObject*>& OutTouched = OutTouchedObjects ? *OutTouchedObjects : LocalTouched;
		const bool bMutating = IsMutatingFunction(Function);

		if (Target == nullptr || Function == nullptr)
		{
			OutResult->SetStringField(TEXT("error_code"), TEXT("OPERATION_FAILED"));
			OutResult->SetStringField(TEXT("error"), TEXT("No target or function to call."));
			OutError = TEXT("No target or function to call.");
			return false;
		}

		// The frame must be constructed and destroyed as a unit: these signatures
		// carry arrays, strings and FText, which leak if it is freed without
		// DestroyStruct. Every early exit below goes through the single destroy at
		// the bottom rather than returning from inside the loop.
		TArray<uint8> Frame;
		Frame.SetNumZeroed(Function->ParmsSize);
		Function->InitializeStruct(Frame.GetData());

		bool bOk = true;

		for (TFieldIterator<FProperty> It(Function); It && bOk; ++It)
		{
			FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Parm) || Property->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			void* Slot = Property->ContainerPtrToValuePtr<void>(Frame.GetData());
			const TSharedPtr<FJsonValue> Supplied = Params.IsValid()
				? Params->TryGetField(Property->GetName())
				: nullptr;

			if (FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(Property))
			{
				UClass* ParamClass = AsObject->PropertyClass;

				if (Policy.AutoSupply)
				{
					if (UObject* Auto = Policy.AutoSupply(ParamClass))
					{
						AsObject->SetObjectPropertyValue(Slot, Auto);
						continue;
					}
				}

				FString Ref;
				const bool bHasRef = Supplied.IsValid() && Supplied->TryGetString(Ref) && !Ref.IsEmpty();

				if (!bHasRef)
				{
					const bool bIsSubject = Policy.DefaultSubject != nullptr
						&& Policy.DefaultSubjectClass != nullptr
						&& ParamClass != nullptr
						&& ParamClass->IsChildOf(Policy.DefaultSubjectClass);
					if (bIsSubject)
					{
						if (bMutating && !Property->HasAnyPropertyFlags(CPF_ConstParm))
						{
							Policy.DefaultSubject->Modify();
							OutTouched.AddUnique(Policy.DefaultSubject);
						}
						AsObject->SetObjectPropertyValue(Slot, Policy.DefaultSubject);
						continue;
					}
					if (Property->HasAnyPropertyFlags(CPF_OutParm))
					{
						// Pure out-parameter; the callee fills it.
						continue;
					}
					OutResult->SetStringField(TEXT("error_code"), TEXT("MISSING_PARAM"));
					OutResult->SetStringField(TEXT("error"),
						FString::Printf(TEXT("Parameter '%s' needs a %s and none was given."),
							*Property->GetName(),
							ParamClass != nullptr ? *ParamClass->GetName() : TEXT("object")));
					OutError = FString::Printf(TEXT("Missing object for parameter '%s'."), *Property->GetName());
					bOk = false;
					break;
				}

				FString ResolveError;
				UObject* Resolved = Policy.ResolveObject
					? Policy.ResolveObject(Ref, ParamClass, ResolveError)
					: nullptr;
				if (Resolved == nullptr)
				{
					OutResult->SetStringField(TEXT("error_code"), TEXT("NOT_FOUND"));
					OutResult->SetStringField(TEXT("error"), ResolveError.IsEmpty()
						? FString::Printf(TEXT("Parameter '%s': could not resolve '%s'."),
							*Property->GetName(), *Ref)
						: ResolveError);
					OutError = FString::Printf(TEXT("Could not resolve '%s' for parameter '%s'."),
						*Ref, *Property->GetName());
					bOk = false;
					break;
				}
				// Record the pre-state before the callee mutates it. Without this the
				// enclosing transaction has nothing to restore and the operation is
				// silently not undoable -- which looks identical to a working undo
				// until someone actually presses Ctrl+Z.
				if (bMutating && !Property->HasAnyPropertyFlags(CPF_ConstParm))
				{
					Resolved->Modify();
					OutTouched.AddUnique(Resolved);
				}
				AsObject->SetObjectPropertyValue(Slot, Resolved);
				continue;
			}

			if (!Supplied.IsValid())
			{
				// Leave the engine default. InitializeStruct already ran the struct
				// constructors, so options structs arrive fully defaulted.
				continue;
			}

			if (!FJsonObjectConverter::JsonValueToUProperty(Supplied, Property, Slot, 0, 0))
			{
				OutResult->SetStringField(TEXT("error_code"), TEXT("INVALID_PARAM"));
				OutResult->SetStringField(TEXT("error"),
					FString::Printf(TEXT("Parameter '%s' (%s) could not be read from the value given."),
						*Property->GetName(), *Property->GetCPPType()));
				OutError = FString::Printf(TEXT("Bad value for parameter '%s'."), *Property->GetName());
				bOk = false;
				break;
			}
		}

		if (bOk)
		{
			Target->ProcessEvent(Function, Frame.GetData());

			for (TFieldIterator<FProperty> It(Function); It; ++It)
			{
				FProperty* Property = *It;
				if (!IsOutputParam(Property))
				{
					continue;
				}
				void* Slot = Property->ContainerPtrToValuePtr<void>(Frame.GetData());

				if (const FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(Property))
				{
					UObject* Returned = AsObject->GetObjectPropertyValue(Slot);
					if (Returned == nullptr)
					{
						OutResult->SetField(Property->GetName(), MakeShared<FJsonValueNull>());
						continue;
					}
					const FString Handle = Policy.MintHandle ? Policy.MintHandle(Returned) : FString();
					OutResult->SetStringField(Property->GetName(),
						Handle.IsEmpty() ? Returned->GetPathName() : Handle);
					continue;
				}
				TSharedPtr<FJsonValue> Exported = FJsonObjectConverter::UPropertyToJsonValue(Property, Slot, 0, 0);
				if (Exported.IsValid())
				{
					OutResult->SetField(Property->GetName(), Exported);
				}
			}
		}

		Function->DestroyStruct(Frame.GetData());

		if (bOk && PostInvoke)
		{
			const int32 DomainErrors = PostInvoke(OutResult);
			if (DomainErrors > 0)
			{
				OutResult->SetStringField(TEXT("error_code"), TEXT("OPERATION_FAILED"));
				OutError = FString::Printf(TEXT("'%s' reported %d error(s)."),
					*Function->GetName(), DomainErrors);
				bOk = false;
			}
		}

		OutResult->SetBoolField(TEXT("mutating"), bMutating);
		OutResult->SetBoolField(TEXT("ok"), bOk);
		return bOk;
	}
}
