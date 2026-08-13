// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "MVVMViewModelBase.h"
#include "ElemagicSkillBarViewModel.generated.h"

class UElementSystemComponent;

/**
 * 技能栏 ViewModel：把 4 槽技能数据扁平化为独立标量 FieldNotify 属性。
 * （MVVM 不支持绑定结构体成员的嵌套字段路径，故拆成标量。）
 */
UCLASS(BlueprintType)
class ELEMAGIC_API UElemagicSkillBarViewModel : public UMVVMViewModelBase
{
	GENERATED_BODY()

public:
	void BindToElementSystem(UElementSystemComponent* InComponent);
	void RefreshFromComponent();

	// === Getters ===
	FText GetSkillSlot0Name() const { return SkillSlot0Name; }
	bool GetSkillSlot0IsEmpty() const { return SkillSlot0IsEmpty; }
	FText GetSkillSlot1Name() const { return SkillSlot1Name; }
	bool GetSkillSlot1IsEmpty() const { return SkillSlot1IsEmpty; }
	FText GetSkillSlot2Name() const { return SkillSlot2Name; }
	bool GetSkillSlot2IsEmpty() const { return SkillSlot2IsEmpty; }
	FText GetSkillSlot3Name() const { return SkillSlot3Name; }
	bool GetSkillSlot3IsEmpty() const { return SkillSlot3IsEmpty; }

	// === Setters ===
	void SetSkillSlot0Name(const FText& V) { UE_MVVM_SET_PROPERTY_VALUE(SkillSlot0Name, V); }
	void SetSkillSlot0IsEmpty(bool V) { UE_MVVM_SET_PROPERTY_VALUE(SkillSlot0IsEmpty, V); }
	void SetSkillSlot1Name(const FText& V) { UE_MVVM_SET_PROPERTY_VALUE(SkillSlot1Name, V); }
	void SetSkillSlot1IsEmpty(bool V) { UE_MVVM_SET_PROPERTY_VALUE(SkillSlot1IsEmpty, V); }
	void SetSkillSlot2Name(const FText& V) { UE_MVVM_SET_PROPERTY_VALUE(SkillSlot2Name, V); }
	void SetSkillSlot2IsEmpty(bool V) { UE_MVVM_SET_PROPERTY_VALUE(SkillSlot2IsEmpty, V); }
	void SetSkillSlot3Name(const FText& V) { UE_MVVM_SET_PROPERTY_VALUE(SkillSlot3Name, V); }
	void SetSkillSlot3IsEmpty(bool V) { UE_MVVM_SET_PROPERTY_VALUE(SkillSlot3IsEmpty, V); }

private:
	TWeakObjectPtr<UElementSystemComponent> Component;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	FText SkillSlot0Name;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	bool SkillSlot0IsEmpty = true;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	FText SkillSlot1Name;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	bool SkillSlot1IsEmpty = true;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	FText SkillSlot2Name;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	bool SkillSlot2IsEmpty = true;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	FText SkillSlot3Name;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	bool SkillSlot3IsEmpty = true;
};
