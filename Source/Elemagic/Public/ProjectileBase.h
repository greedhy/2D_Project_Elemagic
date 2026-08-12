// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayEffect.h"
#include "ProjectileBase.generated.h"

class UBoxComponent;
class UProjectileMovementComponent;
class UPaperFlipbookComponent;

/**
 * 投射物基类：由 CGF_Projectile GA 生成，使用 UProjectileMovementComponent 飞行。
 * 碰撞体使用 Hitbox 通道（ECC_GameTraceChannel1），仅与 Hurtbox（ECC_GameTraceChannel2）重叠。
 * 伤害通过 UElemagicDamageStatics::ApplyDamageToTarget 施加。
 */
UCLASS()
class ELEMAGIC_API AProjectileBase : public AActor
{
	GENERATED_BODY()

public:
	AProjectileBase();

	/**
	 * 由生成方 GA 在 SpawnActor 后立即调用，配置投射物参数。
	 */
	void InitializeProjectile(
		TSubclassOf<UGameplayEffect> InDamageEffectClass,
		float InDamageMultiplier,
		FVector2D InHitImpulse,
		FVector2D InLaunchVelocity,
		float InGravityScale,
		float InLifespan
	);

	/** true = 命中后销毁（默认），false = 穿透（可命中多个目标，每个目标只命中一次） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Projectile")
	bool bDestroyOnHit = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Projectile")
	TObjectPtr<UBoxComponent> CollisionBox;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Projectile")
	TObjectPtr<UProjectileMovementComponent> ProjectileMovement;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Projectile")
	TObjectPtr<UPaperFlipbookComponent> Sprite;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnProjectileOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

private:
	/** 伤害 GE 类（SetByCaller[Data.Damage] 模式） */
	UPROPERTY()
	TSubclassOf<UGameplayEffect> DamageEffectClass;

	/** 伤害倍率 */
	float DamageMultiplier = 1.f;

	/** 击退力（X=水平, Y=垂直） */
	FVector2D HitImpulse = FVector2D::ZeroVector;

	/** 本次飞行中已命中的目标（去重） */
	TSet<TWeakObjectPtr<AActor>> HitTargets;

	/** 超时自毁定时器 */
	FTimerHandle LifespanTimer;
};
