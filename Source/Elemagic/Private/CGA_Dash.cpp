// Fill out your copyright notice in the Description page of Project Settings.

#include "CGA_Dash.h"
#include "CharacterBase.h"
#include "ElemagicGameplayTags.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "AbilitySystemComponent.h"

UCGA_Dash::UCGA_Dash()
{
    AbilityTags.AddTag(ElemagicGameplayTags::Ability_Dash);

    ActivationOwnedTags.AddTag(ElemagicGameplayTags::State_Dashing);

    ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_Dashing);
    ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_DashedInAir);
}

void UCGA_Dash::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    const FGameplayEventData* TriggerEventData)
{
    if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }

    UAbilitySystemComponent* ASC = ActorInfo->AbilitySystemComponent.Get();
    ACharacter* Character = Cast<ACharacter>(ActorInfo->AvatarActor.Get());
    if (!ASC || !Character)
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }

    ASC->AddLooseGameplayTag(ElemagicGameplayTags::State_Invulnerable);

    if (IsInAir())
    {
        ASC->AddLooseGameplayTag(ElemagicGameplayTags::State_DashedInAir);
    }

    UWorld* World = GetWorld();
    if (World)
    {
        World->GetTimerManager().SetTimer(DashTickTimer, this, &UCGA_Dash::DashTick, 0.001f, true);
        World->GetTimerManager().SetTimer(DashEndTimer,
            [this, Handle, ActorInfo, ActivationInfo]()
            {
                EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
            },
            DashDuration, false);
    }

    DashTick();
}

void UCGA_Dash::EndAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    bool bReplicateCancel, bool bEndedByCancel)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(DashTickTimer);
        World->GetTimerManager().ClearTimer(DashEndTimer);
    }

    if (UAbilitySystemComponent* ASC = ActorInfo->AbilitySystemComponent.Get())
    {
        ASC->RemoveLooseGameplayTag(ElemagicGameplayTags::State_Invulnerable);
    }

    Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateCancel, bEndedByCancel);
}

void UCGA_Dash::DashTick()
{
    ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
    if (!Character) return;

    UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement();
    if (!MoveComp) return;

    MoveComp->Velocity.X = GetDashDirectionSign() * DashSpeed;
}

int32 UCGA_Dash::GetDashDirectionSign() const
{
    const ACharacterBase* CharBase = Cast<ACharacterBase>(GetAvatarActorFromActorInfo());
    if (CharBase)
    {
        return CharBase->IsFacingRight() ? 1 : -1;
    }
    return 1;
}

bool UCGA_Dash::IsInAir() const
{
    const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
    if (!Character) return false;

    const UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement();
    return MoveComp && MoveComp->IsFalling();
}
