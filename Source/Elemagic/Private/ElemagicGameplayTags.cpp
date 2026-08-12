// Fill out your copyright notice in the Description page of Project Settings.

#include "ElemagicGameplayTags.h"

namespace ElemagicGameplayTags
{
	UE_DEFINE_GAMEPLAY_TAG(Ability_Attack_Light, "Ability.Attack.Light");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Attack_Heavy, "Ability.Attack.Heavy");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Jump, "Ability.Jump");

	UE_DEFINE_GAMEPLAY_TAG(State_Dead, "State.Dead");
	UE_DEFINE_GAMEPLAY_TAG(State_Attacking, "State.Attacking");

	UE_DEFINE_GAMEPLAY_TAG(Ability_Dash, "Ability.Dash");

	UE_DEFINE_GAMEPLAY_TAG(State_Dashing, "State.Dashing");
	UE_DEFINE_GAMEPLAY_TAG(State_DashedInAir, "State.DashedInAir");
	UE_DEFINE_GAMEPLAY_TAG(State_Invulnerable, "State.Invulnerable");
	UE_DEFINE_GAMEPLAY_TAG(Cooldown_Dash, "Cooldown.Dash");

	UE_DEFINE_GAMEPLAY_TAG(Data_Damage, "Data.Damage");

	UE_DEFINE_GAMEPLAY_TAG(Event_Attack_EnableHitbox, "Event.Attack.EnableHitbox");
	UE_DEFINE_GAMEPLAY_TAG(Event_Attack_DisableHitbox, "Event.Attack.DisableHitbox");
	UE_DEFINE_GAMEPLAY_TAG(Event_Attack_ResetHitTargets, "Event.Attack.ResetHitTargets");
	UE_DEFINE_GAMEPLAY_TAG(Event_Attack_EndAttack, "Event.Attack.EndAttack");

	UE_DEFINE_GAMEPLAY_TAG(State_Hurt, "State.Hurt");
	UE_DEFINE_GAMEPLAY_TAG(State_Knockback, "State.Knockback");
}
