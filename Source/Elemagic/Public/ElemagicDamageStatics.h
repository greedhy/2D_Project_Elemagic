// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GameplayEffect.h"
#include "ElemagicDamageStatics.generated.h"

/**
 * 共享伤害工具：将 ApplyDamage 逻辑从 HitboxManager 提取为静态方法，
 * 供近战（HitboxManager）和远程（ProjectileBase）共用。
 */
UCLASS()
class ELEMAGIC_API UElemagicDamageStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 对目标 Actor 施加伤害。
	 *
	 * @param SourceActor     伤害来源 Actor（用于读取 AttackPower 和 ASC）
	 * @param TargetActor     受击目标 Actor
	 * @param DamageEffectClass SetByCaller[Data.Damage] 的 GE 类
	 * @param DamageMultiplier 伤害倍率（叠加在 AttackPower 上）
	 * @param KnockbackImpulse 击退力（X=水平, Y=垂直），零向量 = 无击退
	 * @param KnockbackDirectionSign 击退水平方向符号（+1=右, -1=左）
	 * @return 实际造成的 FinalDamage（>=0），被阻挡时返回 -1.f
	 */
	static float ApplyDamageToTarget(
		AActor* SourceActor,
		AActor* TargetActor,
		TSubclassOf<UGameplayEffect> DamageEffectClass,
		float DamageMultiplier,
		FVector2D KnockbackImpulse,
		float KnockbackDirectionSign
	);
};
