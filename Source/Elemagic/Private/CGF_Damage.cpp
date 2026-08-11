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
    AbilityTags.AddTag(ElemagicGameplayTags::Ability_Attack);
    ActivationOwnedTags.AddTag(ElemagicGameplayTags::State_Attacking);
    ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_Attacking);
}

void UCGF_Damage::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    const FGameplayEventData* TriggerEventData)
{
    if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }

    // 校验:FrameData 的 SourceAnimation 应与 AttackAnimation 匹配
    if (FrameData && AttackAnimation &&
        FrameData->SourceAnimation.LoadSynchronous() != AttackAnimation)
    {
        UE_LOG(LogTemp, Warning, TEXT("[CGF_Damage] FrameData.SourceAnimation mismatch with AttackAnimation for %s"),
            *GetName());
    }

    UHitboxManager* HitboxMan = GetHitboxManager();
    if (!HitboxMan || !FrameData)
    {
        UE_LOG(LogTemp, Warning, TEXT("[CGF_Damage] HitboxManager or FrameData is null, ending ability"));
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }

    // 将数据喂给 HitboxManager
    HitboxMan->BeginAttack(FrameData, DamageEffectClass, BaseImpulse);

    // 播放动画(纯视觉)
    ACharacterBase* CharBase = Cast<ACharacterBase>(ActorInfo->AvatarActor.Get());
    if (CharBase && AttackAnimation)
    {
        if (UPaperFlipbookComponent* Sprite = CharBase->GetSprite())
        {
            Sprite->SetFlipbook(AttackAnimation);
            Sprite->PlayFromStart();
        }
    }
}

void UCGF_Damage::EndAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    bool bReplicateCancel, bool bEndedByCancel)
{
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
