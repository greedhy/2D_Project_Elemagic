// Fill out your copyright notice in the Description page of Project Settings.

#include "ElemagicGameplayAbility.h"
#include "ElemagicGameplayTags.h"

UElemagicGameplayAbility::UElemagicGameplayAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_Dead);
}
