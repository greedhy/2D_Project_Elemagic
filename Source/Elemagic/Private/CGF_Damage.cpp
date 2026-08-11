// Fill out your copyright notice in the Description page of Project Settings.

#include "CGF_Damage.h"
#include "CharacterBase.h"
#include "HitboxManager.h"
#include "AttackFrameData.h"
#include "ElemagicGameplayTags.h"
#include "PaperFlipbookComponent.h"
#include "PaperFlipbook.h"
#include "AbilitySystemComponent.h"

UCGF_Damage::UCGF_Damage()
{
    // 注意:AbilityTags(如 Ability.Attack)由 BP 子类在 Class Defaults 中配置,
    // 框架基类不硬编码任何 Ability Tag。
    ActivationOwnedTags.AddTag(ElemagicGameplayTags::State_Attacking);
    ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_Attacking);
}

void UCGF_Damage::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    const FGameplayEventData* TriggerEventData)
{
    UE_LOG(LogTemp, Log, TEXT("[CGF_Damage] ActivateAbility START: %s"), *GetName());

    if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
    {
        UE_LOG(LogTemp, Warning, TEXT("[CGF_Damage] CommitAbility FAILED for %s"), *GetName());
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }
    UE_LOG(LogTemp, Log, TEXT("[CGF_Damage] CommitAbility OK"));

    // 打印关键配置的当前值
    UE_LOG(LogTemp, Log, TEXT("[CGF_Damage] AttackAnimation=%s FrameData=%s DamageEffectClass=%s"),
        *GetNameSafe(AttackAnimation),
        *GetNameSafe(FrameData),
        *GetNameSafe(DamageEffectClass));

    // 校验:FrameData 的 SourceAnimation 应与 AttackAnimation 匹配
    if (FrameData && AttackAnimation &&
        FrameData->SourceAnimation.LoadSynchronous() != AttackAnimation)
    {
        UE_LOG(LogTemp, Warning, TEXT("[CGF_Damage] FrameData.SourceAnimation mismatch with AttackAnimation for %s"),
            *GetName());
    }

    UHitboxManager* HitboxMan = GetHitboxManager();
    if (!HitboxMan)
    {
        UE_LOG(LogTemp, Warning, TEXT("[CGF_Damage] HitboxManager is null for %s"), *GetName());
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }
    UE_LOG(LogTemp, Log, TEXT("[CGF_Damage] HitboxManager found"));

    if (!FrameData)
    {
        UE_LOG(LogTemp, Warning, TEXT("[CGF_Damage] FrameData is null — no attack frame config, ending"));
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }
    UE_LOG(LogTemp, Log, TEXT("[CGF_Damage] FrameData OK, Frames.Num=%d"), FrameData->Frames.Num());

    // 将数据喂给 HitboxManager
    HitboxMan->BeginAttack(FrameData, DamageEffectClass, BaseImpulse);
    UE_LOG(LogTemp, Log, TEXT("[CGF_Damage] HitboxManager->BeginAttack called"));

    // 播放动画(纯视觉)
    ACharacterBase* CharBase = Cast<ACharacterBase>(ActorInfo->AvatarActor.Get());
    if (!CharBase)
    {
        UE_LOG(LogTemp, Warning, TEXT("[CGF_Damage] AvatarActor is not ACharacterBase"));
        return;
    }

    if (!AttackAnimation)
    {
        UE_LOG(LogTemp, Warning, TEXT("[CGF_Damage] AttackAnimation is null — no Flipbook to play"));
        return;
    }

    UPaperFlipbookComponent* Sprite = CharBase->GetSprite();
    if (!Sprite)
    {
        UE_LOG(LogTemp, Warning, TEXT("[CGF_Damage] Sprite component is null"));
        return;
    }

    Sprite->SetFlipbook(AttackAnimation);
    Sprite->PlayFromStart();
    UE_LOG(LogTemp, Log, TEXT("[CGF_Damage] Playing Flipbook: %s"), *GetNameSafe(AttackAnimation));

    // 按动画时长设置 EndAbility 定时器
    const float Duration = FrameData->GetTotalDuration();
    UE_LOG(LogTemp, Log, TEXT("[CGF_Damage] Setting EndAbility timer: %.2fs"), Duration);
    UWorld* World = GetWorld();
    if (World)
    {
        World->GetTimerManager().SetTimer(AttackEndTimer,
            [this, Handle, ActorInfo, ActivationInfo]()
            {
                EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
            },
            Duration, false);
    }
}

void UCGF_Damage::EndAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    bool bReplicateCancel, bool bEndedByCancel)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(AttackEndTimer);
    }

    if (UHitboxManager* HitboxMan = GetHitboxManager())
    {
        HitboxMan->EndAttack();
    }

    Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateCancel, bEndedByCancel);
}

UHitboxManager* UCGF_Damage::GetHitboxManager() const
{
    const ACharacterBase* CharBase = Cast<ACharacterBase>(GetAvatarActorFromActorInfo());
    if (CharBase)
    {
        return CharBase->HitboxManager;
    }
    return nullptr;
}
