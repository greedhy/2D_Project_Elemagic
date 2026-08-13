// Fill out your copyright notice in the Description page of Project Settings.

#include "ElemagicGameplayTags.h"

namespace ElemagicGameplayTags
{
	UE_DEFINE_GAMEPLAY_TAG(Ability_Attack_Light, "Ability.Attack.Light");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Attack_Heavy, "Ability.Attack.Heavy");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Jump, "Ability.Jump");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Attack_Projectile, "Ability.Attack.Projectile");

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
	UE_DEFINE_GAMEPLAY_TAG(Combo_WindowOpen, "Combo.WindowOpen");

	UE_DEFINE_GAMEPLAY_TAG(Element_Fire, "Element.Fire");
	UE_DEFINE_GAMEPLAY_TAG(Element_Water, "Element.Water");
	UE_DEFINE_GAMEPLAY_TAG(Element_Earth, "Element.Earth");
	UE_DEFINE_GAMEPLAY_TAG(Element_Wind, "Element.Wind");
	UE_DEFINE_GAMEPLAY_TAG(Element_Lightning, "Element.Lightning");
	UE_DEFINE_GAMEPLAY_TAG(Element_Light, "Element.Light");
	UE_DEFINE_GAMEPLAY_TAG(Element_Dark, "Element.Dark");

	UE_DEFINE_GAMEPLAY_TAG(Input_Skill1, "Input.Skill1");
	UE_DEFINE_GAMEPLAY_TAG(Input_Skill2, "Input.Skill2");
	UE_DEFINE_GAMEPLAY_TAG(Input_Skill3, "Input.Skill3");
	UE_DEFINE_GAMEPLAY_TAG(Input_Skill4, "Input.Skill4");
	UE_DEFINE_GAMEPLAY_TAG(Input_Synthesize, "Input.Synthesize");
}
