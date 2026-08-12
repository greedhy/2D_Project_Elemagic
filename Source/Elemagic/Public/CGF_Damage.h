// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ElemagicGameplayAbility.h"
#include "GameplayTagContainer.h"
#include "CGF_Damage.generated.h"

class UPaperFlipbook;
class UAttackFrameData;
class UGameplayEffect;
class UHitboxManager;
class ACharacterBase;

/**
 * 近战攻击能力基类(CGF = C++ GameplayForm)。
 * 纯编排器:激活时把 FrameData/DamageGE/BaseImpulse 喂给 HitboxManager,
 * 播放 AttackAnimation(纯视觉),命中判定由 HitboxManager 的计时器驱动。
 */
UCLASS()
class ELEMAGIC_API UCGF_Damage : public UElemagicGameplayAbility
{
    GENERATED_BODY()

public:
    UCGF_Damage();

    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayAbilityActivationInfo ActivationInfo,
        const FGameplayEventData* TriggerEventData) override;

    virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayAbilityActivationInfo ActivationInfo,
        bool bReplicateCancel, bool bEndedByCancel) override;

    // 攻击动画 Flipbook(纯视觉播放,不驱动判定)
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Damage")
    TObjectPtr<UPaperFlipbook> AttackAnimation;

    // 帧数据资产
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Damage")
    TObjectPtr<UAttackFrameData> FrameData;

    // 伤害 GE 类(通过 SetByCaller[Data.Damage] 传递伤害值)
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Damage")
    TSubclassOf<UGameplayEffect> DamageEffectClass;

    // 底板击退力(每帧 HitImpulse 叠加在此之上)
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Damage")
    FVector2D BaseImpulse = FVector2D::ZeroVector;

    // 子类自定义 Ability Tag(如 Ability.Attack.Light)
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Damage")
    FGameplayTag AbilityAttackTag;

    // === Combo ===

    // Combo 窗口开始时间（归一化 0.0~1.0），此时间后按键触发下一段。≤0 = 无 combo
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
    float ComboWindowStart = 0.f;

    // Combo 链的下一段技能类（为空 = 终结技 / 无 combo）
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
    TSubclassOf<class UCGF_Damage> NextComboAbilityClass;

    // Combo 窗口监听的输入 Tag（默认空 = 取 AbilityTags 的第一个 Tag）
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
    FGameplayTag ComboInputTag;

private:
    UHitboxManager* GetHitboxManager() const;

    void OpenComboWindow();
    void TryActivateNextCombo();

    FTimerHandle AttackEndTimer;
    FTimerHandle ComboWindowTimer;

    bool bComboWindowOpened = false;
};
