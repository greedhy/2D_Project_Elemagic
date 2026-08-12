// Fill out your copyright notice in the Description page of Project Settings.

#include "CGF_Damage.h"
#include "CharacterBase.h"
#include "HitboxManager.h"
#include "AttackFrameData.h"
#include "MyPlayerController.h"
#include "ElemagicGameplayTags.h"
#include "PaperFlipbookComponent.h"
#include "PaperFlipbook.h"
#include "AbilitySystemComponent.h"

UCGF_Damage::UCGF_Damage()
{
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

    HitboxMan->BeginAttack(FrameData, DamageEffectClass, BaseImpulse);

    ACharacterBase* CharBase = Cast<ACharacterBase>(ActorInfo->AvatarActor.Get());
    if (CharBase && AttackAnimation)
    {
        if (UPaperFlipbookComponent* Sprite = CharBase->GetSprite())
        {
            Sprite->SetFlipbook(AttackAnimation);
            Sprite->PlayFromStart();
        }
    }

    const float Duration = FrameData->GetTotalDuration();
    UWorld* World = GetWorld();
    if (World)
    {
        World->GetTimerManager().SetTimer(AttackEndTimer,
            [this, Handle, ActorInfo, ActivationInfo]()
            {
                EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
            },
            Duration, false);

        if (ComboWindowStart > 0.f && NextComboAbilityClass)
        {
            const float WindowTime = ComboWindowStart * Duration;
            World->GetTimerManager().SetTimer(ComboWindowTimer,
                this, &UCGF_Damage::OpenComboWindow,
                WindowTime, false);
        }
    }
}

void UCGF_Damage::OpenComboWindow()
{
    bComboWindowOpened = true;

    UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
    if (ASC)
    {
        ASC->AddLooseGameplayTag(ElemagicGameplayTags::Combo_WindowOpen);
    }
}

void UCGF_Damage::TryActivateNextCombo()
{
    UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
    if (!ASC || !NextComboAbilityClass)
    {
        return;
    }

    ASC->RemoveLooseGameplayTag(ElemagicGameplayTags::Combo_WindowOpen);
    ASC->TryActivateAbilityByClass(NextComboAbilityClass);
}

void UCGF_Damage::EndAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    bool bReplicateCancel, bool bEndedByCancel)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(AttackEndTimer);
        World->GetTimerManager().ClearTimer(ComboWindowTimer);
    }

    if (UHitboxManager* HitboxMan = GetHitboxManager())
    {
        HitboxMan->EndAttack();
    }

    if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
    {
        ASC->RemoveLooseGameplayTag(ElemagicGameplayTags::Combo_WindowOpen);
    }

    // 检查 combo 缓冲输入 — combo window 内缓冲的输入在 EndAbility 时消费
    bool bShouldChainCombo = false;
    if (!bEndedByCancel && bComboWindowOpened)
    {
        AMyPlayerController* PC = Cast<AMyPlayerController>(ActorInfo->PlayerController.Get());
        if (PC && PC->HasBufferedComboInput())
        {
            const FGameplayTag ComboTag = ComboInputTag.IsValid()
                ? ComboInputTag
                : (AbilityTags.IsEmpty() ? FGameplayTag() : *AbilityTags.CreateConstIterator());

            if (PC->ConsumeBufferedComboInput(ComboTag))
            {
                bShouldChainCombo = true;
            }
        }
    }

    // 先结束当前技能（释放 GAS slot + 移除 State.Attacking）
    Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateCancel, bEndedByCancel);

    // 再激活下一段
    if (bShouldChainCombo)
    {
        TryActivateNextCombo();
    }
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
