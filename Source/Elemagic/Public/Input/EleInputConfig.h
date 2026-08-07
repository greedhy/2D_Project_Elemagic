// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "EleInputConfig.generated.h"

class UInputAction;

USTRUCT(BlueprintType)
struct FEleInputAction
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly)
	TObjectPtr<UInputAction> InputAction = nullptr;

	UPROPERTY(EditDefaultsOnly)
	FGameplayTag InputTag = FGameplayTag();
};

/**
 * GameplayTag -> InputAction 的映射表(数据资源)。
 * 新增一个技能输入只需要在蓝图里创建/编辑本类的实例,添加一条 (InputAction, InputTag) 记录,
 * 不需要新增任何 C++ 代码或改动 PlayerController。
 */
UCLASS()
class ELEMAGIC_API UEleInputConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	const UInputAction* FindAbilityInputActionForTag(const FGameplayTag& InputTag, bool bLogNotFound = false) const;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TArray<FEleInputAction> AbilityInputActions;
};
