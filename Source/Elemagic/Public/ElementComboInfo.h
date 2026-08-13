// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "ElementComboInfo.generated.h"

class UGameplayAbility;
class UTexture2D;

/**
 * 元素组合表的一行：3 个元素 → 一个技能。
 * Elements 是集合语义（顺序无关，不区分重复元素）。
 */
USTRUCT(BlueprintType)
struct FElementComboEntry
{
	GENERATED_BODY()

	// 正好 3 个元素 Tag（顺序无关）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	FGameplayTagContainer Elements;

	// 合成得到的技能类
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	TSubclassOf<UGameplayAbility> ResultAbility;

	// 显示名（技能栏 UI）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	FText DisplayName;

	// 图标（技能栏 UI）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	TObjectPtr<UTexture2D> Icon;

	// 冷却 Tag（技能冷却 GE 授予，Tick 中查询剩余时间）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
	FGameplayTag CooldownTag;
};

/**
 * 元素组合表 DataAsset：数据驱动 7 元素 → 技能 的映射。
 * 与 test_25d 的 UAbilityInfo 模式一致（标签 → 能力类查找）。
 */
UCLASS(BlueprintType)
class ELEMAGIC_API UElementComboInfo : public UDataAsset
{
	GENERATED_BODY()

public:
	// 按元素集合查找组合（顺序无关 + 数量守卫）。找不到返回 nullptr。
	// 注意：返回 USTRUCT 指针，故不暴露给蓝图（仅供 C++ 内部调用）。
	const FElementComboEntry* FindCombo(const FGameplayTagContainer& Elements) const;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combo")
	TArray<FElementComboEntry> Combos;
};
