// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "GameplayTagContainer.h"
#include "CharacterAttributeSetBase.generated.h"

#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/**
 * 角色通用属性集:生命值、攻击力、防御力、移动速度。
 * 供玩家与敌人共用,后续可派生子类添加职业/元素专属属性。
 */
UCLASS()
class ELEMAGIC_API UCharacterAttributeSetBase : public UAttributeSet
{
	GENERATED_BODY()

public:
	UCharacterAttributeSetBase();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Health, Category = "Attributes")
	FGameplayAttributeData Health;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, Health)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxHealth, Category = "Attributes")
	FGameplayAttributeData MaxHealth;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, MaxHealth)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_AttackPower, Category = "Attributes")
	FGameplayAttributeData AttackPower;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, AttackPower)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Defense, Category = "Attributes")
	FGameplayAttributeData Defense;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, Defense)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MoveSpeed, Category = "Attributes")
	FGameplayAttributeData MoveSpeed;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, MoveSpeed)

	// 管道属性:SetByCaller[Data.Damage] 把伤害写入此属性,
	// PostGameplayEffectExecute 中消费并扣减 Health 后归零。
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_IncomingDamage, Category = "Attributes")
	FGameplayAttributeData IncomingDamage;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, IncomingDamage)

	// === 元素抗性（7 种，值域 0~1，0=无抗性 1=免疫）===
	// 现在声明，Phase 5 元素伤害时消耗。

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_FireResistance, Category = "Attributes|Element")
	FGameplayAttributeData FireResistance;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, FireResistance)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_WaterResistance, Category = "Attributes|Element")
	FGameplayAttributeData WaterResistance;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, WaterResistance)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_EarthResistance, Category = "Attributes|Element")
	FGameplayAttributeData EarthResistance;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, EarthResistance)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_WindResistance, Category = "Attributes|Element")
	FGameplayAttributeData WindResistance;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, WindResistance)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_LightningResistance, Category = "Attributes|Element")
	FGameplayAttributeData LightningResistance;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, LightningResistance)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_LightResistance, Category = "Attributes|Element")
	FGameplayAttributeData LightResistance;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, LightResistance)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_DarkResistance, Category = "Attributes|Element")
	FGameplayAttributeData DarkResistance;
	ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, DarkResistance)

	// 按元素 Tag 返回对应抗性值（未知 Tag 返回 0）
	UFUNCTION(BlueprintPure, Category = "Attributes|Element")
	float GetResistanceForElement(FGameplayTag ElementTag) const;

protected:
	UFUNCTION()
	virtual void OnRep_Health(const FGameplayAttributeData& OldHealth);

	UFUNCTION()
	virtual void OnRep_MaxHealth(const FGameplayAttributeData& OldMaxHealth);

	UFUNCTION()
	virtual void OnRep_AttackPower(const FGameplayAttributeData& OldAttackPower);

	UFUNCTION()
	virtual void OnRep_Defense(const FGameplayAttributeData& OldDefense);

	UFUNCTION()
	virtual void OnRep_MoveSpeed(const FGameplayAttributeData& OldMoveSpeed);

	UFUNCTION()
	virtual void OnRep_IncomingDamage(const FGameplayAttributeData& OldIncomingDamage);

	UFUNCTION()
	virtual void OnRep_FireResistance(const FGameplayAttributeData& OldFireResistance);

	UFUNCTION()
	virtual void OnRep_WaterResistance(const FGameplayAttributeData& OldWaterResistance);

	UFUNCTION()
	virtual void OnRep_EarthResistance(const FGameplayAttributeData& OldEarthResistance);

	UFUNCTION()
	virtual void OnRep_WindResistance(const FGameplayAttributeData& OldWindResistance);

	UFUNCTION()
	virtual void OnRep_LightningResistance(const FGameplayAttributeData& OldLightningResistance);

	UFUNCTION()
	virtual void OnRep_LightResistance(const FGameplayAttributeData& OldLightResistance);

	UFUNCTION()
	virtual void OnRep_DarkResistance(const FGameplayAttributeData& OldDarkResistance);

private:
	void ClampHealthAttribute(float& Value) const;
};
