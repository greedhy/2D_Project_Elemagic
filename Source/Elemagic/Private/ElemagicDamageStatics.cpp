// Fill out your copyright notice in the Description page of Project Settings.

#include "ElemagicDamageStatics.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "CharacterAttributeSetBase.h"
#include "CharacterBase.h"
#include "ElemagicGameplayTags.h"
#include "GameplayEffect.h"
#include "PaperFlipbook.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

float UElemagicDamageStatics::ApplyDamageToTarget(
	AActor* SourceActor,
	AActor* TargetActor,
	TSubclassOf<UGameplayEffect> DamageEffectClass,
	float DamageMultiplier,
	FVector2D KnockbackImpulse,
	float KnockbackDirectionSign)
{
	// ---- Step 1: 获取 Source 和 Target 的 ASC ----
	UAbilitySystemComponent* SourceASC = SourceActor
		? SourceActor->FindComponentByClass<UAbilitySystemComponent>()
		: nullptr;
	UAbilitySystemComponent* TargetASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(TargetActor);
	if (!SourceASC || !TargetASC)
	{
		return -1.f;
	}

	// ---- Step 2: 从源角色属性集读 AttackPower，计算最终伤害 ----
	float AttackPower = 0.f;
	if (const UCharacterAttributeSetBase* AttrSet = SourceASC->GetSet<UCharacterAttributeSetBase>())
	{
		AttackPower = AttrSet->GetAttackPower();
	}
	const float FinalDamage = AttackPower * DamageMultiplier;

	// ---- Step 3: 无敌拦截：目标有 State_Invulnerable 时跳过伤害 ----
	if (TargetASC->HasMatchingGameplayTag(ElemagicGameplayTags::State_Invulnerable))
	{
		return -1.f;
	}

	// ---- Step 4: 源 ASC 创建 GE Spec，通过 SetByCaller 传递伤害值 ----
	FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(
		DamageEffectClass, 1.f, SourceASC->MakeEffectContext());

	if (SpecHandle.IsValid())
	{
		SpecHandle.Data->SetSetByCallerMagnitude(ElemagicGameplayTags::Data_Damage, FinalDamage);
		TargetASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
	}

	// ---- Step 5: 给目标添加受击状态 ----
	TargetASC->AddLooseGameplayTag(ElemagicGameplayTags::State_Hurt);

	// ---- Step 6: 可选 iFrame：仅当目标角色配置了 HurtIFrameDuration > 0 时启用 ----
	if (ACharacterBase* TargetCharBase = Cast<ACharacterBase>(TargetActor))
	{
		if (TargetCharBase->HurtIFrameDuration > 0.f)
		{
			TargetCharBase->StartHurtIFrame(TargetCharBase->HurtIFrameDuration);
		}
	}

	// ---- Step 7: 施加击退 ----
	if (!KnockbackImpulse.IsNearlyZero() && TargetActor)
	{
		if (ACharacter* TargetChar = Cast<ACharacter>(TargetActor))
		{
			if (UCharacterMovementComponent* MoveComp = TargetChar->GetCharacterMovement())
			{
				MoveComp->Velocity += FVector(
					KnockbackImpulse.X * KnockbackDirectionSign,
					0.f,
					KnockbackImpulse.Y
				);
			}
		}
	}

	// ---- Step 8: 受击动画播完后移除 State_Hurt ----
	if (ACharacterBase* TargetCharBase = Cast<ACharacterBase>(TargetActor))
	{
		const float HurtAnimDuration = TargetCharBase->HurtFlipbook
			? TargetCharBase->HurtFlipbook->GetTotalDuration()
			: 0.2f;

		if (UWorld* World = TargetActor->GetWorld())
		{
			FTimerHandle HurtEndTimer;
			World->GetTimerManager().SetTimer(HurtEndTimer,
				[TargetASC]()
				{
					if (TargetASC)
					{
						TargetASC->RemoveLooseGameplayTag(ElemagicGameplayTags::State_Hurt);
					}
				},
				HurtAnimDuration, false);
		}
	}
	else
	{
		TargetASC->RemoveLooseGameplayTag(ElemagicGameplayTags::State_Hurt);
	}

	return FinalDamage;
}
