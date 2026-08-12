// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ElemagicGameplayAbility.h"
#include "GameplayTagContainer.h"
#include "GameplayEffect.h"
#include "CGF_Projectile.generated.h"

class AProjectileBase;
class UPaperFlipbook;

/**
 * 远程投射物能力基类（CGF = C++ GameplayForm）。
 * 纯编排器：激活时基于角色朝向生成 AProjectileBase 实例并配置飞行参数，
 * 可选播放 FireAnimation（发射动画）。投射物独立飞行，GA 在发射动画结束后自行结束。
 *
 * Blueprint 子类通过配置 ProjectileClass / DamageEffectClass / LaunchVelocity 等属性，
 * 可快速产出直线弹道（Fireball）、抛物线（Arrow）、AoE（Bomb）等投射物技能。
 */
UCLASS()
class ELEMAGIC_API UCGF_Projectile : public UElemagicGameplayAbility
{
	GENERATED_BODY()

public:
	UCGF_Projectile();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateCancel, bool bEndedByCancel) override;

	// === Blueprint 可配置属性 ===

	/** 投射物 Actor 类（BP 子类） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Projectile")
	TSubclassOf<AProjectileBase> ProjectileClass;

	/** 伤害 GE 类（通过 SetByCaller[Data.Damage] 传递伤害值） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Projectile")
	TSubclassOf<UGameplayEffect> DamageEffectClass;

	/** 生成偏移（相对角色位置，X=水平, Y=垂直） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Projectile")
	FVector2D SpawnOffset = FVector2D(64.f, 0.f);

	/** 初速度（X=水平, Y=垂直，会根据角色朝向自动翻转 X） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Projectile")
	FVector2D LaunchVelocity = FVector2D(800.f, 0.f);

	/** 重力缩放（0=直线弹道, >0=抛物线） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Projectile")
	float GravityScale = 0.f;

	/** 投射物最大存活时间（秒） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Projectile")
	float ProjectileLifespan = 5.f;

	/** 伤害倍率（叠加在 AttackPower 上） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Projectile")
	float DamageMultiplier = 1.f;

	/** 命中击退力（X=水平, Y=垂直） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Projectile")
	FVector2D HitImpulse = FVector2D(200.f, 100.f);

	/** 子类自定义 Ability Tag（如 Ability.Attack.Projectile） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Projectile")
	FGameplayTag AbilityProjectileTag;

	/** 发射动画 Flipbook（纯视觉，为空则不播放动画） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Projectile")
	TObjectPtr<UPaperFlipbook> FireAnimation;

private:
	FTimerHandle FireEndTimer;
};
