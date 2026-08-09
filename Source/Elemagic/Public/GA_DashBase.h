// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ElemagicGameplayAbility.h"
#include "GA_DashBase.generated.h"

/**
 * 冲刺能力骨架:面朝方向水平位移,带无敌帧。
 * 冷却时长/空中次数等配置由蓝图子类在 Class Defaults 中设置。
 * 输入通过 InputConfig 的 Ability.Dash -> ASC::TryActivateAbilitiesByTag 路由。
 */
UCLASS()
class ELEMAGIC_API UGA_DashBase : public UElemagicGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_DashBase();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateCancel, bool bEndedByCancel) override;

	// 冲刺速度(单位/秒):2667 = 400 / 0.15
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Dash")
	float DashSpeed = 2667.f;

	// 冲刺持续时长(秒)
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Dash")
	float DashDuration = 0.15f;

private:
	void DashTick();
	int32 GetDashDirectionSign() const;
	bool IsInAir() const;

	FTimerHandle DashTickTimer;
	FTimerHandle DashEndTimer;
};
