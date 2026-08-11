// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "HitboxManager.generated.h"

class UBoxComponent;
class UAttackFrameData;
class UGameplayEffect;

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAttackHit, AActor* /*Target*/, float /*FinalDamage*/);

UCLASS(ClassGroup = (Elemagic), meta = (BlueprintSpawnableComponent))
class ELEMAGIC_API UHitboxManager : public UActorComponent
{
    GENERATED_BODY()

public:
    UHitboxManager();

    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    // 由 ACharacterBase 构造函数调用,绑定碰撞体
    void Init(UBoxComponent* InAttackHitbox, UBoxComponent* InHurtbox);

    // 开始一次攻击,喂入 FrameData + DamageGE + BaseImpulse
    void BeginAttack(UAttackFrameData* Data, TSubclassOf<UGameplayEffect> DamageGE, FVector2D BaseImpulse);

    // 结束攻击(关闭 Hitbox + 清空去重集合)
    void EndAttack();

    // AttackHitbox 碰到其他角色 Hurtbox 的回调
    UFUNCTION()
    void OnAttackHitboxOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
        UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

    FOnAttackHit OnAttackHit;

protected:
    UPROPERTY()
    TObjectPtr<UBoxComponent> AttackHitbox;

    UPROPERTY()
    TObjectPtr<UBoxComponent> Hurtbox;

private:
    void ProcessFrameConfig(const struct FAttackFrameConfig& Config);
    void ApplyDamage(AActor* Target, float DamageMultiplier);

    UPROPERTY()
    TObjectPtr<UAttackFrameData> CurrentFrameData;

    UPROPERTY()
    TSubclassOf<UGameplayEffect> CurrentDamageEffectClass;

    FVector2D CurrentBaseImpulse = FVector2D::ZeroVector;

    bool bActive = false;
    float StartTime = 0.f;
    float TotalDuration = 0.f;
    TSet<TWeakObjectPtr<AActor>> HitTargets;
    int32 LastProcessedConfigIdx = 0;

    // 当前激活帧的伤害倍率与击退(Overlap 回调时读取)
    float CurrentDamageMultiplier = 1.f;
    FVector2D CurrentHitImpulse = FVector2D::ZeroVector;
};
