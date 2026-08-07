// Fill out your copyright notice in the Description page of Project Settings.

#include "GA_Attack.h"
#include "ElemagicGameplayTags.h"

UGA_Attack::UGA_Attack()
{
	AbilityTags.AddTag(ElemagicGameplayTags::Ability_Attack);
	ActivationOwnedTags.AddTag(ElemagicGameplayTags::State_Attacking);
	ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_Attacking);
}

void UGA_Attack::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	PerformAttack();
}
