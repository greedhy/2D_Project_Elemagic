// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ElemagicGameplayAbility.h"
#include "GA_Dash.generated.h"

/**
 * 冲刺能力:面朝方向 0.15s / 400 单位水平位移,带无敌帧。
 * 地面有 GAS 冷却(空中清零),空中每滞空限 1 次(State_DashedInAir tag 阻挡)。
 * 输入通过 InputConfig 的 Ability.Dash -> ASC::TryActivateAbilitiesByTag 路由。
 */
UCLASS()
class ELEMAGIC_API UGA_Dash : public UElemagicGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_Dash();

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
