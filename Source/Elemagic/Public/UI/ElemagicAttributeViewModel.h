// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "MVVMViewModelBase.h"
#include "ElemagicAttributeViewModel.generated.h"

class UAbilitySystemComponent;
struct FOnAttributeChangeData;

/**
 * 属性 ViewModel：绑定到 UAbilitySystemComponent，把 UCharacterAttributeSetBase 的
 * 属性值镜像为 FieldNotify 属性，供血条/属性列表/怪物血条共用。
 * 一个实例服务多个 View（血条 + 属性列表 + 怪物血条）。
 */
UCLASS(BlueprintType)
class ELEMAGIC_API UElemagicAttributeViewModel : public UMVVMViewModelBase
{
	GENERATED_BODY()

public:
	// 绑定到指定 ASC（订阅属性变化委托并同步初始值）。
	void BindToAbilitySystem(UAbilitySystemComponent* InASC);
	void UnbindFromAbilitySystem();

	virtual void BeginDestroy() override;

	// === Setters（仅 C++ 调用，由属性变化委托驱动）===
	void SetHealth(float NewHealth);
	void SetMaxHealth(float NewMaxHealth);
	void SetAttackPower(float NewAttackPower);
	void SetDefense(float NewDefense);
	void SetMoveSpeed(float NewMoveSpeed);
	void SetFireResistance(float NewFireResistance);
	void SetWaterResistance(float NewWaterResistance);
	void SetEarthResistance(float NewEarthResistance);
	void SetWindResistance(float NewWindResistance);
	void SetLightningResistance(float NewLightningResistance);
	void SetLightResistance(float NewLightResistance);
	void SetDarkResistance(float NewDarkResistance);

	// === Getters（UI 绑定用）===
	float GetHealth() const { return Health; }
	float GetMaxHealth() const { return MaxHealth; }

	UFUNCTION(BlueprintPure, FieldNotify)
	float GetHealthPercent() const { return MaxHealth > 0.f ? Health / MaxHealth : 0.f; }

	float GetAttackPower() const { return AttackPower; }
	float GetDefense() const { return Defense; }
	float GetMoveSpeed() const { return MoveSpeed; }
	float GetFireResistance() const { return FireResistance; }
	float GetWaterResistance() const { return WaterResistance; }
	float GetEarthResistance() const { return EarthResistance; }
	float GetWindResistance() const { return WindResistance; }
	float GetLightningResistance() const { return LightningResistance; }
	float GetLightResistance() const { return LightResistance; }
	float GetDarkResistance() const { return DarkResistance; }

private:
	void OnAttributeChanged(const FOnAttributeChangeData& Data);

	TWeakObjectPtr<UAbilitySystemComponent> ASC;
	TArray<FDelegateHandle> DelegateHandles;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float Health = 0.f;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float MaxHealth = 0.f;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float AttackPower = 0.f;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float Defense = 0.f;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float MoveSpeed = 0.f;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float FireResistance = 0.f;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float WaterResistance = 0.f;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float EarthResistance = 0.f;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float WindResistance = 0.f;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float LightningResistance = 0.f;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float LightResistance = 0.f;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	float DarkResistance = 0.f;
};
