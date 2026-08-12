// Fill out your copyright notice in the Description page of Project Settings.

#include "HitboxManager.h"
#include "AttackFrameData.h"
#include "CharacterBase.h"
#include "ElemagicDamageStatics.h"
#include "ElemagicGameplayTags.h"
#include "CharacterAttributeSetBase.h"
#include "PaperFlipbook.h"
#include "Components/BoxComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameplayEffect.h"
#include "Engine/World.h"
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
    // 仅服务端执行伤害判定 — 客户端物理世界不同步
    if (!GetOwner() || !GetOwner()->HasAuthority())
    {
        return;
    }

    if (!bActive || !OtherActor || !CurrentDamageEffectClass)
    {
        return;
    }

    // 不攻击自己
    if (OtherActor == GetOwner())
    {
        return;
    }

    // 检查 OtherComp 是否是 Hurtbox 通道
    if (!OtherComp || OtherComp->GetCollisionObjectType() != ECC_GameTraceChannel2)
    {
        return;
    }

    // 去重:本段内同一目标只命中一次
    if (HitTargets.Contains(OtherActor))
    {
        return;
    }

    ApplyDamage(OtherActor, CurrentDamageMultiplier);
    HitTargets.Add(OtherActor);
}

void UHitboxManager::ApplyDamage(AActor* Target, float DamageMultiplier)
{
	// 按源角色朝向方向确定击退水平方向符号
	const float DirectionSign = (GetOwner() && GetOwner()->GetActorForwardVector().X > 0.f) ? 1.f : -1.f;

	// 最终击退 = BaseImpulse + 当前帧 HitImpulse
	const FVector2D FinalImpulse = CurrentBaseImpulse + CurrentHitImpulse;

	// 委托给共享静态工具（提取为独立函数供 ProjectileBase 复用）
	const float FinalDamage = UElemagicDamageStatics::ApplyDamageToTarget(
		GetOwner(),
		Target,
		CurrentDamageEffectClass,
		DamageMultiplier,
		FinalImpulse,
		DirectionSign
	);

	// 广播命中事件
	if (FinalDamage >= 0.f)
	{
		OnAttackHit.Broadcast(Target, FinalDamage);
	}
}
