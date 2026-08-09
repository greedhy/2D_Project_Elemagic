// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ElemagicGameplayAbility.h"
#include "GA_AttackBase.generated.h"

/**
 * 基础近战攻击能力骨架。原生代码只负责提交与状态标签管理,
 * 具体的命中判定/受击特效由蓝图子类通过 PerformAttack 实现,
 * 并在动画结束时调用 K2_EndAbility 结束能力。
 */
UCLASS()
class ELEMAGIC_API UGA_AttackBase : public UElemagicGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_AttackBase();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

protected:
	UFUNCTION(BlueprintImplementableEvent, Category = "Attack")
	void PerformAttack();
};
