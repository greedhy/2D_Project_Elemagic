// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "MVVMViewModelBase.h"
#include "ElemagicElementLoadoutViewModel.generated.h"

class UElementSystemComponent;

/**
 * 元素装载 ViewModel：镜像 3 槽装载 + 可合成状态。
 * 元素槽以 Tag 名称字符串暴露（空串 = 空槽），便于直接绑定 Text 显示。
 */
UCLASS(BlueprintType)
class ELEMAGIC_API UElemagicElementLoadoutViewModel : public UMVVMViewModelBase
{
	GENERATED_BODY()

public:
	void BindToElementSystem(UElementSystemComponent* InComponent);
	void RefreshFromComponent();

	// Getters
	FString GetElementSlot0() const { return ElementSlot0; }
	FString GetElementSlot1() const { return ElementSlot1; }
	FString GetElementSlot2() const { return ElementSlot2; }
	bool GetCanSynthesize() const { return bCanSynthesize; }

	// Setters
	void SetElementSlot0(const FString& V) { UE_MVVM_SET_PROPERTY_VALUE(ElementSlot0, V); }
	void SetElementSlot1(const FString& V) { UE_MVVM_SET_PROPERTY_VALUE(ElementSlot1, V); }
	void SetElementSlot2(const FString& V) { UE_MVVM_SET_PROPERTY_VALUE(ElementSlot2, V); }
	void SetCanSynthesize(bool V) { UE_MVVM_SET_PROPERTY_VALUE(bCanSynthesize, V); }

private:
	TWeakObjectPtr<UElementSystemComponent> Component;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	FString ElementSlot0;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	FString ElementSlot1;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	FString ElementSlot2;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter = "GetCanSynthesize", meta = (AllowPrivateAccess = "true"))
	bool bCanSynthesize = false;
};
