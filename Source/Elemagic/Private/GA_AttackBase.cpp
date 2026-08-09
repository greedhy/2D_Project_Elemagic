// Fill out your copyright notice in the Description page of Project Settings.

#include "GA_AttackBase.h"
#include "ElemagicGameplayTags.h"

UGA_AttackBase::UGA_AttackBase()
{
	AbilityTags.AddTag(ElemagicGameplayTags::Ability_Attack);
	ActivationOwnedTags.AddTag(ElemagicGameplayTags::State_Attacking);
	ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_Attacking);
}

void UGA_AttackBase::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	PerformAttack();
}
