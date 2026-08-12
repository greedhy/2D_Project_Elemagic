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
            UE_LOG(LogTemp, Log, TEXT("[Combo] %s: scheduling combo window at %.2fs (%.0f%% of %.2fs) -> %s"),
                *GetName(), WindowTime, ComboWindowStart * 100.f, Duration,
                *GetNameSafe(NextComboAbilityClass.Get()));
            World->GetTimerManager().SetTimer(ComboWindowTimer,
                this, &UCGF_Damage::OpenComboWindow,
                WindowTime, false);
        }
        else
        {
            UE_LOG(LogTemp, Log, TEXT("[Combo] %s: NO combo configured (Start=%.2f Next=%s)"),
                *GetName(), ComboWindowStart, *GetNameSafe(NextComboAbilityClass.Get()));
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
        UE_LOG(LogTemp, Log, TEXT("[Combo] Window OPEN for %s, tag added to %s"),
            *GetName(), *GetNameSafe(ASC->GetOwner()));
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("[Combo] OpenComboWindow: No ASC! for %s"), *GetName());
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
    UE_LOG(LogTemp, Log, TEXT("[Combo] Chaining %s -> %s"), *GetName(), *GetNameSafe(NextComboAbilityClass.Get()));
    ASC->TryActivateAbilityByClass(NextComboAbilityClass);
}

void UCGF_Damage::EndAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    bool bReplicateCancel, bool bEndedByCancel)
{
    UE_LOG(LogTemp, Log, TEXT("[Combo] EndAbility %s: bComboWindowOpened=%d bEndedByCancel=%d"),
        *GetName(), bComboWindowOpened, bEndedByCancel);

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

    bool bShouldChainCombo = false;
    if (!bEndedByCancel && bComboWindowOpened)
    {
        AMyPlayerController* PC = Cast<AMyPlayerController>(ActorInfo->PlayerController.Get());
        const bool bHasBuffered = PC && PC->HasBufferedComboInput();

        UE_LOG(LogTemp, Log, TEXT("[Combo] EndAbility %s: PC=%s HasBuffered=%d"),
            *GetName(), *GetNameSafe(PC), bHasBuffered);

        if (PC && bHasBuffered)
        {
            const FGameplayTag ComboTag = ComboInputTag.IsValid()
                ? ComboInputTag
                : (AbilityTags.IsEmpty() ? FGameplayTag() : *AbilityTags.CreateConstIterator());

            UE_LOG(LogTemp, Log, TEXT("[Combo] EndAbility: ComboTag=%s BufferedTag=%s"),
                *ComboTag.ToString(), *PC->GetBufferedInputTag().ToString());

            if (PC->ConsumeBufferedComboInput(ComboTag))
            {
                bShouldChainCombo = true;
                UE_LOG(LogTemp, Log, TEXT("[Combo] WILL chain to next combo!"));
            }
            else
            {
                UE_LOG(LogTemp, Log, TEXT("[Combo] Tag mismatch or expired"));
            }
        }
    }

    Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateCancel, bEndedByCancel);

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
