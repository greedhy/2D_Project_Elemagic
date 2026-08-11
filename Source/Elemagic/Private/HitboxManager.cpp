// Fill out your copyright notice in the Description page of Project Settings.

#include "HitboxManager.h"
#include "AttackFrameData.h"
#include "ElemagicGameplayTags.h"
#include "CharacterAttributeSetBase.h"
#include "Components/BoxComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameplayEffect.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

UHitboxManager::UHitboxManager()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UHitboxManager::Init(UBoxComponent* InAttackHitbox, UBoxComponent* InHurtbox)
{
    AttackHitbox = InAttackHitbox;
    Hurtbox = InHurtbox;

    if (AttackHitbox)
    {
        AttackHitbox->OnComponentBeginOverlap.AddDynamic(this, &UHitboxManager::OnAttackHitboxOverlap);
    }
}

void UHitboxManager::BeginAttack(UAttackFrameData* Data, TSubclassOf<UGameplayEffect> DamageGE, FVector2D BaseImpulse)
{
    if (!Data || Data->Frames.Num() == 0)
    {
        return;
    }

    CurrentFrameData = Data;
    CurrentDamageEffectClass = DamageGE;
    CurrentBaseImpulse = BaseImpulse;
    TotalDuration = Data->GetTotalDuration();
    StartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
    LastProcessedConfigIdx = 0;
    HitTargets.Reset();
    bActive = true;

    SetComponentTickEnabled(true);
}

void UHitboxManager::EndAttack()
{
    bActive = false;
    SetComponentTickEnabled(false);

    if (AttackHitbox)
    {
        AttackHitbox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }

    CurrentFrameData = nullptr;
    CurrentDamageEffectClass = nullptr;
    CurrentBaseImpulse = FVector2D::ZeroVector;
    CurrentDamageMultiplier = 1.f;
    CurrentHitImpulse = FVector2D::ZeroVector;
    HitTargets.Reset();
    LastProcessedConfigIdx = 0;
}

void UHitboxManager::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!bActive || !CurrentFrameData || !GetWorld())
    {
        return;
    }

    const float Elapsed = GetWorld()->GetTimeSeconds() - StartTime;
    const float NormalizedTime = FMath::Clamp(Elapsed / TotalDuration, 0.f, 1.f);

    const TArray<FAttackFrameConfig>& Frames = CurrentFrameData->Frames;

    // 增量处理从上次游标到当前时间区间内的所有配置节点
    for (int32 i = LastProcessedConfigIdx; i < Frames.Num(); ++i)
    {
        if (Frames[i].NormalizedTime <= NormalizedTime)
        {
            ProcessFrameConfig(Frames[i]);
            LastProcessedConfigIdx = i + 1;
        }
        else
        {
            break;
        }
    }

    // 动画播放完毕自动结束
    if (NormalizedTime >= 1.f)
    {
        EndAttack();
    }
}

void UHitboxManager::ProcessFrameConfig(const FAttackFrameConfig& Config)
{
    if (!AttackHitbox)
    {
        return;
    }

    if (Config.EventTags.HasTagExact(ElemagicGameplayTags::Event_Attack_EnableHitbox))
    {
        UE_LOG(LogTemp, Log, TEXT("[HitboxManager] EnableHitbox at NormalizedTime=%.2f, Extent=(%.0f,%.0f) Offset=(%.0f,%.0f)"),
            Config.NormalizedTime, Config.HitboxExtent.X, Config.HitboxExtent.Y, Config.HitboxOffset.X, Config.HitboxOffset.Y);
        AttackHitbox->SetBoxExtent(FVector(Config.HitboxExtent.X, 1.f, Config.HitboxExtent.Y));
        AttackHitbox->SetRelativeLocation(FVector(Config.HitboxOffset.X, 0.f, Config.HitboxOffset.Y));
        AttackHitbox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        CurrentDamageMultiplier = Config.DamageMultiplier;
        CurrentHitImpulse = Config.HitImpulse;
    }

    if (Config.EventTags.HasTagExact(ElemagicGameplayTags::Event_Attack_DisableHitbox))
    {
        AttackHitbox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        CurrentDamageMultiplier = 1.f;
        CurrentHitImpulse = FVector2D::ZeroVector;
    }

    if (Config.EventTags.HasTagExact(ElemagicGameplayTags::Event_Attack_ResetHitTargets))
    {
        HitTargets.Reset();
    }

    if (Config.EventTags.HasTagExact(ElemagicGameplayTags::Event_Attack_EndAttack))
    {
        EndAttack();
    }
}

void UHitboxManager::OnAttackHitboxOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
    UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
    UE_LOG(LogTemp, Log, TEXT("[HitboxManager] Overlap: Other=%s OtherComp=%s bActive=%d"),
        *GetNameSafe(OtherActor), *GetNameSafe(OtherComp), bActive);

    if (!bActive || !OtherActor || !CurrentDamageEffectClass)
    {
        UE_LOG(LogTemp, Log, TEXT("[HitboxManager] Rejected: bActive=%d OtherActor=%s DamageEffectClass=%s"),
            bActive, *GetNameSafe(OtherActor), *GetNameSafe(CurrentDamageEffectClass.Get()));
        return;
    }

    // 不攻击自己
    if (OtherActor == GetOwner())
    {
        UE_LOG(LogTemp, Log, TEXT("[HitboxManager] Rejected: self-hit"));
        return;
    }

    // 检查 OtherComp 是否是 Hurtbox 通道
    if (!OtherComp || OtherComp->GetCollisionObjectType() != ECC_GameTraceChannel2)
    {
        UE_LOG(LogTemp, Log, TEXT("[HitboxManager] Rejected: OtherComp channel=%d (expected %d)"),
            OtherComp ? (int32)OtherComp->GetCollisionObjectType() : -1, (int32)ECC_GameTraceChannel2);
        return;
    }

    // 去重:本段内同一目标只命中一次
    if (HitTargets.Contains(OtherActor))
    {
        UE_LOG(LogTemp, Log, TEXT("[HitboxManager] Rejected: already hit %s this attack"), *GetNameSafe(OtherActor));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("[HitboxManager] HIT! Target=%s DamageMultiplier=%.2f"), *GetNameSafe(OtherActor), CurrentDamageMultiplier);
    ApplyDamage(OtherActor, CurrentDamageMultiplier);
    HitTargets.Add(OtherActor);
}

void UHitboxManager::ApplyDamage(AActor* Target, float DamageMultiplier)
{
    UAbilitySystemComponent* SourceASC = GetOwner()
        ? GetOwner()->FindComponentByClass<UAbilitySystemComponent>()
        : nullptr;
    UAbilitySystemComponent* TargetASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Target);
    if (!SourceASC || !TargetASC)
    {
        UE_LOG(LogTemp, Warning, TEXT("[HitboxManager] ApplyDamage FAILED: SourceASC=%s TargetASC=%s"),
            *GetNameSafe(SourceASC), *GetNameSafe(TargetASC));
        return;
    }

    // 从源角色属性集读 AttackPower,计算最终伤害
    float AttackPower = 0.f;
    if (const UCharacterAttributeSetBase* AttrSet = SourceASC->GetSet<UCharacterAttributeSetBase>())
    {
        AttackPower = AttrSet->GetAttackPower();
    }
    const float FinalDamage = AttackPower * DamageMultiplier;

    UE_LOG(LogTemp, Log, TEXT("[HitboxManager] ApplyDamage: AttackPower=%.1f x Multiplier=%.2f = FinalDamage=%.1f"),
        AttackPower, DamageMultiplier, FinalDamage);

    // 源 ASC 创建 GE Spec,通过 SetByCaller 传递伤害值
    FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(
        CurrentDamageEffectClass, 1.f, SourceASC->MakeEffectContext());

    if (SpecHandle.IsValid())
    {
        SpecHandle.Data->SetSetByCallerMagnitude(ElemagicGameplayTags::Data_Damage, FinalDamage);
        TargetASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());

        // 读取目标血量和伤害结果
        if (const UCharacterAttributeSetBase* TargetAttr = TargetASC->GetSet<UCharacterAttributeSetBase>())
        {
            UE_LOG(LogTemp, Log, TEXT("[HitboxManager] Target Health: %.1f -> %.1f (after %.1f damage)"),
                TargetAttr->GetHealth() + FinalDamage, TargetAttr->GetHealth(), FinalDamage); // 注意:PostGameplayEffectExecute 可能还没跑完,这里是近似值
        }

        UE_LOG(LogTemp, Log, TEXT("[HitboxManager] GE applied to %s, FinalDamage=%.1f via SetByCaller[Data.Damage]"),
            *GetNameSafe(Target), FinalDamage);

        OnAttackHit.Broadcast(Target, FinalDamage);
    }

    // 施加击退:最终击退 = BaseImpulse + 当前帧 HitImpulse
    const FVector2D FinalImpulse = CurrentBaseImpulse + CurrentHitImpulse;
    if (!FinalImpulse.IsNearlyZero() && Target)
    {
        if (ACharacter* TargetChar = Cast<ACharacter>(Target))
        {
            if (UCharacterMovementComponent* MoveComp = TargetChar->GetCharacterMovement())
            {
                // 按源角色朝向方向施加击退
                const float DirectionSign = (GetOwner() && GetOwner()->GetActorForwardVector().X > 0.f) ? 1.f : -1.f;
                MoveComp->Velocity += FVector(FinalImpulse.X * DirectionSign, 0.f, FinalImpulse.Y);
            }
        }
    }
}
