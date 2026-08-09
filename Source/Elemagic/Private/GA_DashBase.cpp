// Fill out your copyright notice in the Description page of Project Settings.

#include "GA_DashBase.h"
#include "CharacterBase.h"
#include "ElemagicGameplayTags.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "AbilitySystemComponent.h"

UGA_DashBase::UGA_DashBase()
{
	AbilityTags.AddTag(ElemagicGameplayTags::Ability_Dash);

	ActivationOwnedTags.AddTag(ElemagicGameplayTags::State_Dashing);

	ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_Dashing);
	ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_DashedInAir);
}

void UGA_DashBase::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
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
		World->GetTimerManager().SetTimer(DashTickTimer, this, &UGA_DashBase::DashTick, 0.001f, true);
		World->GetTimerManager().SetTimer(DashEndTimer,
			[this, Handle, ActorInfo, ActivationInfo]()
			{
				EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
			},
			DashDuration, false);
	}

	// 第一帧立即推一次速度,不等 Timer 首 tick 延迟
	DashTick();
}

void UGA_DashBase::EndAbility(const FGameplayAbilitySpecHandle Handle,
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

void UGA_DashBase::DashTick()
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character) return;

	UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement();
	if (!MoveComp) return;

	MoveComp->Velocity.X = GetDashDirectionSign() * DashSpeed;
}

int32 UGA_DashBase::GetDashDirectionSign() const
{
	const ACharacterBase* CharBase = Cast<ACharacterBase>(GetAvatarActorFromActorInfo());
	if (CharBase)
	{
		return CharBase->IsFacingRight() ? 1 : -1;
	}
	return 1;
}

bool UGA_DashBase::IsInAir() const
{
	const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character) return false;

	const UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement();
	return MoveComp && MoveComp->IsFalling();
}
