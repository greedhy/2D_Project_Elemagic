// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "AttackFrameData.generated.h"

/**
 * 单个攻击帧配置:定义某个标准化时间点上的 Hitbox 参数/事件/伤害倍率。
 * NormalizedTime 为 0.0~1.0 的比例值,与动画实际时长无关,
 * 实际时间由 Flipbook->GetTotalDuration() * NormalizedTime 计算。
 */
USTRUCT(BlueprintType)
struct FAttackFrameConfig
{
    GENERATED_BODY()

    // 触发时间点(0.0=动画开始, 1.0=动画结束)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    float NormalizedTime = 0.f;

    // 碰撞体半尺寸(X=宽/2, Y=高/2)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    FVector2D HitboxExtent = FVector2D(32.f, 32.f);

    // 碰撞体相对角色中心偏移
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    FVector2D HitboxOffset = FVector2D(0.f, 0.f);

    // 帧事件 Tag(EnableHitbox/DisableHitbox/ResetHitTargets/EndAttack)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    FGameplayTagContainer EventTags;

    // 当前帧命中伤害倍率(叠加到 AttackPower 上)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    float DamageMultiplier = 1.f;

    // 命中击退力(最终击退 = BaseImpulse + HitImpulse)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    FVector2D HitImpulse = FVector2D::ZeroVector;
};

/**
 * 攻击帧数据资产:一个攻击动画对应一个 DataAsset,
 * Frames 按 NormalizedTime 升序排列。
 */
UCLASS(BlueprintType)
class ELEMAGIC_API UAttackFrameData : public UDataAsset
{
    GENERATED_BODY()

public:
    // 配对的 Flipbook,用于获取总时长 + 编辑器对照(不参与运行时播放)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack")
    TSoftObjectPtr<class UPaperFlipbook> SourceAnimation;

    // 帧配置数组,按 NormalizedTime 升序排列
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack")
    TArray<FAttackFrameConfig> Frames;

    // 获取动画总时长(秒),优先从 SourceAnimation 获取,
    // fallback 用最后一帧 NormalizedTime 反算(默认 0.5s)
    float GetTotalDuration() const;
};
