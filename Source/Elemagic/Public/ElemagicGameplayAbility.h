// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "ElemagicGameplayAbility.generated.h"

/**
 * 所有 Elemagic 能力的公共基类,统一实例化/网络策略,
 * 并在角色死亡时(State.Dead)自动阻止激活。
 */
UCLASS()
class ELEMAGIC_API UElemagicGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UElemagicGameplayAbility();
};
